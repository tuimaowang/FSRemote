#include "stream/RemoteVideoScheduler.h"

#include <algorithm>
#include <limits>

namespace stream {

// =====wjy====
void RemoteVideoScheduler::upsert(const RemoteVideoScheduleState& state, std::int64_t nowMs)
{
    if (state.windowId == 0) {
        return;
    }
    const auto existing = entries_.find(state.windowId);
    if (existing == entries_.end()) {
        Entry entry;
        entry.state = state;
        entry.nextDueMs = nowMs;
        entries_.emplace(state.windowId, std::move(entry));
        return;
    }

    Entry& entry = existing->second;
    const bool profileChanged = entry.state.targetFps != state.targetFps
        || entry.state.role != state.role
        || entry.state.generation != state.generation;
    entry.state = state;
    if (profileChanged) {
        entry.nextDueMs = std::min(entry.nextDueMs, nowMs);
        entry.serviceCount = 0;
    }
}

void RemoteVideoScheduler::remove(std::uint64_t windowId)
{
    entries_.erase(windowId);
}

void RemoteVideoScheduler::markFrameReady(std::uint64_t windowId, std::uint64_t generation)
{
    const auto it = entries_.find(windowId);
    if (it == entries_.end() || it->second.state.generation != generation) {
        return;
    }
    it->second.state.frameReady = true;
}

void RemoteVideoScheduler::markPresented(std::uint64_t windowId, std::int64_t nowMs)
{
    const auto it = entries_.find(windowId);
    if (it == entries_.end()) {
        return;
    }
    Entry& entry = it->second;
    entry.state.frameReady = false;
    entry.nextDueMs = nowMs + frameIntervalMs(entry.state.targetFps);
    ++entry.serviceCount;
    roundRobinCursor_ = windowId;
}

void RemoteVideoScheduler::markDropped(std::uint64_t windowId, std::int64_t nowMs)
{
    const auto it = entries_.find(windowId);
    if (it == entries_.end()) {
        return;
    }
    Entry& entry = it->second;
    entry.state.frameReady = false;
    // 丢帧后仍推进 deadline，避免一个忙碌窗口在同一时刻反复抢占 worker。
    entry.nextDueMs = nowMs + frameIntervalMs(entry.state.targetFps);
    ++entry.serviceCount;
    roundRobinCursor_ = windowId;
}

void RemoteVideoScheduler::markRetry(std::uint64_t windowId, std::int64_t nowMs, std::int64_t retryDelayMs)
{
    const auto it = entries_.find(windowId);
    if (it == entries_.end()) {
        return;
    }
    Entry& entry = it->second;
    entry.state.frameReady = true; // wjy: keyed mutex尚未完成交接时保留同一in-flight帧，不能把它误记成已消费丢帧。
    entry.nextDueMs = nowMs + std::max<std::int64_t>(1, retryDelayMs); // wjy: 轻量退避让其它窗口先运行，并等待解码器完成key 1交接。
    roundRobinCursor_ = windowId; // wjy: 重试窗口让出本轮游标，防止同步忙的焦点窗口在同一时刻自旋占满RenderWorker。
}

std::optional<RemoteVideoScheduleDecision> RemoteVideoScheduler::next(std::int64_t nowMs)
{
    Entry* selected = nullptr;
    std::uint64_t selectedId = 0;
    for (auto& [windowId, entry] : entries_) {
        if (!entry.state.frameReady || entry.state.role == RemoteVideoWindowRole::Hidden) {
            continue;
        }
        if (!selected) {
            selected = &entry;
            selectedId = windowId;
            continue;
        }

        const bool entryDue = entry.nextDueMs <= nowMs;
        const bool selectedDue = selected->nextDueMs <= nowMs;
        if (entryDue != selectedDue) {
            if (entryDue) {
                selected = &entry;
                selectedId = windowId;
            }
            continue;
        }

        const int entryRole = roleRank(entry.state.role);
        const int selectedRole = roleRank(selected->state.role);
        if (entryRole != selectedRole) {
            if (entryRole > selectedRole) {
                selected = &entry;
                selectedId = windowId;
            }
            continue;
        }

        if (entry.nextDueMs != selected->nextDueMs) {
            if (entry.nextDueMs < selected->nextDueMs) {
                selected = &entry;
                selectedId = windowId;
            }
            continue;
        }

        // 后台窗口 deadline 相同时，用窗口 ID 轮转，防止 map 遍历顺序造成饥饿。
        const bool entryAfterCursor = windowId > roundRobinCursor_;
        const bool selectedAfterCursor = selectedId > roundRobinCursor_;
        if (entryAfterCursor != selectedAfterCursor) {
            if (entryAfterCursor) {
                selected = &entry;
                selectedId = windowId;
            }
            continue;
        }
        if (entry.state.priority > selected->state.priority
            || (entry.state.priority == selected->state.priority && windowId < selectedId)) {
            selected = &entry;
            selectedId = windowId;
        }
    }

    if (!selected) {
        return std::nullopt;
    }
    if (selected->nextDueMs > nowMs) {
        return std::nullopt; // wjy: deadline未到时必须停止本轮调度，真正落实焦点60 FPS和后台30 FPS本地呈现上限。
    }
    RemoteVideoScheduleDecision decision;
    decision.windowId = selectedId;
    decision.generation = selected->state.generation;
    decision.deadlineMs = selected->nextDueMs;
    decision.priority = selected->state.priority;
    decision.overdue = selected->nextDueMs <= nowMs;
    return decision;
}

std::size_t RemoteVideoScheduler::size() const noexcept
{
    return entries_.size();
}

std::vector<RemoteVideoScheduleState> RemoteVideoScheduler::snapshot() const
{
    std::vector<RemoteVideoScheduleState> states;
    states.reserve(entries_.size());
    for (const auto& [windowId, entry] : entries_) {
        (void)windowId;
        states.push_back(entry.state);
    }
    std::sort(states.begin(), states.end(), [](const auto& left, const auto& right) {
        return left.windowId < right.windowId;
    });
    return states;
}

std::int64_t RemoteVideoScheduler::frameIntervalMs(std::uint32_t targetFps) noexcept
{
    if (targetFps == 0) {
        return std::numeric_limits<std::int64_t>::max() / 4;
    }
    return std::max<std::int64_t>(1, 1000 / static_cast<std::int64_t>(targetFps));
}

int RemoteVideoScheduler::roleRank(RemoteVideoWindowRole role) noexcept
{
    switch (role) {
    case RemoteVideoWindowRole::Focused: return 4;
    case RemoteVideoWindowRole::VisibleBackground: return 3;
    case RemoteVideoWindowRole::Minimized: return 1;
    case RemoteVideoWindowRole::Hidden: return 0;
    }
    return 0;
}
// ===end====

} // namespace stream
