#pragma once

#include "ui/ScriptUiStateStore.h"

#include <QString>

#include <atomic>
#include <memory>

class QTextEdit;

namespace ui {

// =====wjy====
class ScriptPanelController {
protected:
    void saveVisibleState(ScriptUiState& state, QTextEdit* editor) const;
    void applyState(const ScriptUiState& state, const QString& deviceIp, QTextEdit* editor);

protected:
    // wjy: 这些字段只保存当前详情页的脚本面板投影；每设备权威数据仍由 ScriptUiStateStore 持有。
    bool m_scriptOutputVisible = false;
    bool m_scriptOutputRunning = false;
    bool m_scriptOutputFailed = false;
    int m_scriptOutputScrollOffset = 0;
    bool m_scriptOutputAutoScroll = true;
    bool m_scriptOutputDirty = false;
    QString m_scriptOutputTitle;
    QString m_scriptOutputText;
    QString m_scriptOutputFilePath;
    QString m_lastScriptEntryPath; // wjy: 当前详情页待执行的具体入口文件路径，执行时再取其父目录。
    std::shared_ptr<std::atomic_bool> m_scriptCancelRequested;
    bool m_scriptEditorVisible = false;
    bool m_scriptEditorLoading = false;
    bool m_scriptEditorSaving = false;
    QString m_scriptEditorTitle;
    QString m_scriptEditorRemotePath;
    QString m_scriptEditorDeviceIp;
    QString m_scriptEditorLoginUser;
    QString m_scriptEditorWorkName;
};
// ===end====

} // namespace ui
