#pragma once

#include <QHash>
#include <QString>

#include <atomic>
#include <memory>

namespace ui {

// =====wjy====
struct ScriptUiState {
    bool outputVisible = false;
    bool outputRunning = false;
    bool outputFailed = false;
    bool localLaunchInProgress = false; // wjy: 本控制端仍持有启动 SSH 任务时保护短暂的远端清单空窗。
    bool remoteStatusConfirmed = false; // wjy: 标记运行态是否已被目标状态服务确认。
    QString remoteRunId; // wjy: 远端唯一运行 ID，停止和完成回调只能处理同一次任务。
    qint64 remoteControllerPid = 0;
    qint64 remoteStartedAtEpochMs = 0;
    int outputScrollOffset = 0;
    bool outputAutoScroll = true;
    bool outputDirty = false;
    QString outputTitle;
    QString outputText;
    QString outputFilePath;
    QString lastScriptEntryPath; // wjy: 保存用户明确选择的入口文件路径，而不是整个脚本目录。
    std::shared_ptr<std::atomic_bool> cancelRequested;
    bool editorVisible = false;
    bool editorLoading = false;
    bool editorSaving = false;
    QString editorTitle;
    QString editorRemotePath;
    QString editorDeviceIp;
    QString editorLoginUser;
    QString editorWorkName;
    QString editorRequestId; // wjy: 读取/保存请求身份，迟到回调不能覆盖后续操作。
    QString editorText;
    bool editorModified = false;
};

class ScriptUiStateStore final {
public:
    ScriptUiState state(const QString& deviceIp) const;
    void setState(const QString& deviceIp, ScriptUiState state);
    void removeState(const QString& deviceIp);
    void requestCancelAll();

private:
    QHash<QString, ScriptUiState> m_states; // wjy: 每个规范化设备 IP 只有一份权威脚本状态。
};
// ===end====

} // namespace ui
