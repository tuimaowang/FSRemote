#include "ui/ScriptUiStateStore.h"

#include <utility>

namespace ui {

// =====wjy====
ScriptUiState ScriptUiStateStore::state(const QString& deviceIp) const
{
    return m_states.value(deviceIp.trimmed()); // wjy: 所有读写统一规范化 IP，避免空格形成第二份设备状态。
}

void ScriptUiStateStore::setState(const QString& deviceIp, ScriptUiState state)
{
    const QString key = deviceIp.trimmed();
    if (!key.isEmpty()) {
        m_states.insert(key, std::move(state)); // wjy: 整体替换同一设备状态，避免多个平行 QHash 字段错位。
    }
}

void ScriptUiStateStore::removeState(const QString& deviceIp)
{
    m_states.remove(deviceIp.trimmed()); // wjy: 删除设备时同步清理它的脚本输出、编辑器和取消标志。
}

void ScriptUiStateStore::requestCancelAll()
{
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        if (it->cancelRequested) {
            it->cancelRequested->store(true); // wjy: 应用退出时集中取消全部设备的长时间 SSH 脚本任务。
        }
    }
}
// ===end====

} // namespace ui
