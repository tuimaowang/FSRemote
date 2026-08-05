#pragma once

// =====wjy====
#include <atomic>

namespace uu {

struct ControlAdmissionDecision {
    bool granted = false;
    bool claimed_exclusive_slot = false;
};

inline ControlAdmissionDecision decideControlAdmission(
    bool requested_control,
    bool negotiated_control,
    bool exclusive_policy,
    std::atomic_bool* exclusive_slot)
{
    if (!requested_control || !negotiated_control) return {}; // wjy: 未请求 control 或能力协商不包含 control 的会话始终保持只读。
    if (!exclusive_policy) return {true, false}; // wjy: 默认共享策略不争抢全局槽，每个合格认证会话都独立获准。
    bool expected = false;
    const bool claimed = exclusive_slot && exclusive_slot->compare_exchange_strong(expected, true);
    return {claimed, claimed}; // wjy: 仅显式兼容独占策略沿用首会话原子竞争语义。
}
} // namespace uu
// ===end====
