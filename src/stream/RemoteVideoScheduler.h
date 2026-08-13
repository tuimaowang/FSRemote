#pragma once

#include "stream/RemoteVideoPolicy.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace stream {

// =====wjy====
struct RemoteVideoScheduleState {
    std::uint64_t windowId = 0;
    RemoteVideoWindowRole role = RemoteVideoWindowRole::Hidden;
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    std::uint64_t generation = 0;
    bool frameReady = false;
};

struct RemoteVideoScheduleDecision {
    std::uint64_t windowId = 0;
    std::uint64_t generation = 0;
    std::int64_t deadlineMs = 0;
    std::uint32_t priority = 0;
    bool overdue = false;
};

// 单一 RenderWorker 使用的轻量截止时间调度器。
// 它不创建线程、不接触 Qt，只负责把焦点窗口和后台窗口排序出来。
class RemoteVideoScheduler final {
public:
    void upsert(const RemoteVideoScheduleState& state, std::int64_t nowMs);
    void remove(std::uint64_t windowId);
    void markFrameReady(std::uint64_t windowId, std::uint64_t generation);
    void markPresented(std::uint64_t windowId, std::int64_t nowMs);
    void markDropped(std::uint64_t windowId, std::int64_t nowMs);
    void markRetry(std::uint64_t windowId, std::int64_t nowMs, std::int64_t retryDelayMs = 2);

    std::optional<RemoteVideoScheduleDecision> next(std::int64_t nowMs);
    std::size_t size() const noexcept;
    std::vector<RemoteVideoScheduleState> snapshot() const;

private:
    struct Entry {
        RemoteVideoScheduleState state;
        std::int64_t nextDueMs = 0;
        std::uint64_t serviceCount = 0;
    };

    static std::int64_t frameIntervalMs(std::uint32_t targetFps) noexcept;
    static int roleRank(RemoteVideoWindowRole role) noexcept;

    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t roundRobinCursor_ = 0;
};
// ===end====

} // namespace stream
