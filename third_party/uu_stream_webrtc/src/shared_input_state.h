#pragma once

// =====wjy====
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uu {

enum class SharedInputTransition {
    None,
    InjectDown,
    InjectUp,
    InjectRepeat,
}; // wjy: 调度器用明确转换结果区分首次按下、最终抬起、同会话重复按键和应被合并的多人重复状态。

struct SharedInputReleaseBatch {
    std::vector<int> keys;
    std::vector<int> buttons;
}; // wjy: 会话退出时仅返回“已经没有其他持有者”的输入，调用方据此向系统补发安全抬起事件。

class SharedInputState final {
public:
    SharedInputTransition updateKey(const std::string& session_id, int key, bool down)
    {
        return update(session_id, key, down, true, keys_by_session_, key_holders_); // wjy: 键盘允许同一会话的重复 key-down 继续形成系统自动重复语义。
    }

    SharedInputTransition updateButton(const std::string& session_id, int button, bool down)
    {
        return update(session_id, button, down, false, buttons_by_session_, button_holders_); // wjy: 鼠标按钮重复 down 不应重复注入，只维护跨会话持有关系。
    }

    SharedInputReleaseBatch releaseSession(const std::string& session_id)
    {
        SharedInputReleaseBatch released;
        releaseValues(session_id, keys_by_session_, key_holders_, &released.keys); // wjy: 一个控制端断开只减少它持有的键，其他控制端仍按住的键不会被提前抬起。
        releaseValues(session_id, buttons_by_session_, button_holders_, &released.buttons);
        return released;
    }

    // =====wjy====
    std::vector<int> releaseAllButtons()
    {
        std::vector<int> released;
        released.reserve(button_holders_.size()); // wjy: 后端切换只收集当前真正处于按下状态的唯一鼠标按钮，不重复发送多人共享按钮的抬起事件。
        for (const auto& [button, holders] : button_holders_) {
            if (holders > 0) released.push_back(button); // wjy: 持有数大于零才需要向旧注入后端补发一次安全抬起。
        }
        buttons_by_session_.clear(); // wjy: 切换系统鼠标与驱动鼠标后，旧会话的按钮持有关系必须重新从下一次按下开始建立。
        button_holders_.clear();
        return released;
    }

    SharedInputReleaseBatch releaseAll()
    {
        SharedInputReleaseBatch released;
        released.keys.reserve(key_holders_.size());
        released.buttons.reserve(button_holders_.size()); // wjy: 键鼠后端统一切换时预留真实唯一持有量，避免多人共享状态生成重复抬起。
        for (const auto& [key, holders] : key_holders_) {
            if (holders > 0) released.keys.push_back(key); // wjy: 每个仍按住的键只交给旧后端一次 key-up，随后清除全部会话归属。
        }
        for (const auto& [button, holders] : button_holders_) {
            if (holders > 0) released.buttons.push_back(button);
        }
        keys_by_session_.clear();
        buttons_by_session_.clear();
        key_holders_.clear();
        button_holders_.clear(); // wjy: 切换完成后旧会话的 down 不能跨越系统/FakerInput 边界，下一次物理按下重新建状态。
        return released;
    }
    // ===end====

    bool empty() const
    {
        return keys_by_session_.empty() && buttons_by_session_.empty();
    }

private:
    using SessionValues = std::unordered_map<std::string, std::unordered_set<int>>;
    using HolderCounts = std::unordered_map<int, size_t>;

    static SharedInputTransition update(
        const std::string& session_id,
        int value,
        bool down,
        bool repeat_allowed,
        SessionValues& values_by_session,
        HolderCounts& holder_counts)
    {
        if (session_id.empty() || value <= 0) return SharedInputTransition::None; // wjy: 缺少已认证会话 ID 或非法键值时不改变共享状态。
        auto session = values_by_session.find(session_id);
        if (down) {
            if (session == values_by_session.end()) {
                session = values_by_session.emplace(session_id, std::unordered_set<int>{}).first;
            }
            const bool inserted = session->second.insert(value).second;
            if (!inserted) return repeat_allowed ? SharedInputTransition::InjectRepeat : SharedInputTransition::None;
            size_t& holders = holder_counts[value];
            ++holders;
            return holders == 1 ? SharedInputTransition::InjectDown : SharedInputTransition::None; // wjy: 第二个及后续会话按住同一输入时只增加持有数，禁止重复制造系统 down。
        }

        if (session == values_by_session.end() || session->second.erase(value) == 0) {
            return SharedInputTransition::None; // wjy: 从未由该会话按下的 up 不能影响其他控制端的真实持有状态。
        }
        if (session->second.empty()) values_by_session.erase(session);
        auto holder = holder_counts.find(value);
        if (holder == holder_counts.end()) return SharedInputTransition::None;
        if (--holder->second == 0) {
            holder_counts.erase(holder);
            return SharedInputTransition::InjectUp; // wjy: 只有最后一个持有者释放时才向目标操作系统注入 up。
        }
        return SharedInputTransition::None;
    }

    static void releaseValues(
        const std::string& session_id,
        SessionValues& values_by_session,
        HolderCounts& holder_counts,
        std::vector<int>* released)
    {
        const auto session = values_by_session.find(session_id);
        if (session == values_by_session.end()) return;
        for (const int value : session->second) {
            auto holder = holder_counts.find(value);
            if (holder == holder_counts.end()) continue;
            if (--holder->second == 0) {
                holder_counts.erase(holder);
                released->push_back(value); // wjy: 批次只包含本次清理后已无人持有的键或按钮。
            }
        }
        values_by_session.erase(session);
    }

    SessionValues keys_by_session_;
    SessionValues buttons_by_session_;
    HolderCounts key_holders_;
    HolderCounts button_holders_;
};
} // namespace uu
// ===end====
