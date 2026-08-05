#include "ui/ScriptPanelController.h"

#include <QTextDocument>
#include <QTextEdit>

namespace ui {

// =====wjy====
void ScriptPanelController::saveVisibleState(ScriptUiState& state, QTextEdit* editor) const
{
    state.outputVisible = m_scriptOutputVisible;
    state.outputRunning = m_scriptOutputRunning;
    state.outputFailed = m_scriptOutputFailed;
    state.outputScrollOffset = m_scriptOutputScrollOffset;
    state.outputAutoScroll = m_scriptOutputAutoScroll;
    state.outputDirty = m_scriptOutputDirty;
    state.outputTitle = m_scriptOutputTitle;
    state.outputText = m_scriptOutputText;
    state.outputFilePath = m_scriptOutputFilePath;
    state.lastScriptEntryPath = m_lastScriptEntryPath; // wjy: 切换设备时保留具体入口，避免同目录多个脚本恢复后产生歧义。
    state.cancelRequested = m_scriptCancelRequested;
    state.editorVisible = m_scriptEditorVisible;
    state.editorLoading = m_scriptEditorLoading;
    state.editorSaving = m_scriptEditorSaving;
    state.editorTitle = m_scriptEditorTitle;
    state.editorRemotePath = m_scriptEditorRemotePath;
    state.editorDeviceIp = m_scriptEditorDeviceIp;
    state.editorLoginUser = m_scriptEditorLoginUser;
    state.editorWorkName = m_scriptEditorWorkName;
    if (editor) {
        state.editorText = editor->toPlainText(); // wjy: 切换设备前保留尚未提交到远端的编辑器文本。
        state.editorModified = editor->document()->isModified(); // wjy: 同步本地脏标记，避免切换设备后误清除未保存提示。
    }
}

void ScriptPanelController::applyState(const ScriptUiState& state, const QString& deviceIp, QTextEdit* editor)
{
    const QString normalizedIp = deviceIp.trimmed();
    m_scriptOutputVisible = state.outputVisible;
    m_scriptOutputRunning = state.outputRunning;
    m_scriptOutputFailed = state.outputFailed;
    m_scriptOutputScrollOffset = state.outputScrollOffset;
    m_scriptOutputAutoScroll = state.outputAutoScroll;
    m_scriptOutputDirty = state.outputDirty;
    m_scriptOutputTitle = state.outputTitle;
    m_scriptOutputText = state.outputText;
    m_scriptOutputFilePath = state.outputFilePath;
    m_lastScriptEntryPath = state.lastScriptEntryPath; // wjy: 恢复用户最后选择的入口文件，执行函数随后同步它的父目录。
    m_scriptCancelRequested = state.cancelRequested;
    m_scriptEditorVisible = state.editorVisible;
    m_scriptEditorLoading = state.editorLoading;
    m_scriptEditorSaving = state.editorSaving;
    m_scriptEditorTitle = state.editorTitle;
    m_scriptEditorRemotePath = state.editorRemotePath;
    m_scriptEditorDeviceIp = state.editorDeviceIp.trimmed().isEmpty() ? normalizedIp : state.editorDeviceIp;
    m_scriptEditorLoginUser = state.editorLoginUser;
    m_scriptEditorWorkName = state.editorWorkName;
    if (editor) {
        editor->setPlainText(state.editorText); // wjy: 只把目标设备自己的文本投影到当前编辑器。
        editor->document()->setModified(state.editorModified); // wjy: 恢复该设备切换前的本地修改状态。
    }
}
// ===end====

} // namespace ui
