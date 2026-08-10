#pragma once

#include "ui/RemoteInputScript.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace platform {

// =====wjy====
enum class RemoteInputScriptState {
    Unknown,
    Idle,
    Preparing,
    Running,
    WaitingLoop,
    Stopping,
    Failed,
}; // wjy: F9/F10 键鼠脚本拥有独立状态轴，不与主界面右键脚本的进程状态混用。

QString remoteInputScriptStateName(RemoteInputScriptState state);
bool remoteInputScriptStateFromName(const QString& name, RemoteInputScriptState* state);
bool remoteInputScriptStateIsActive(RemoteInputScriptState state);

struct RemoteInputScriptRuntimeInfo {
    bool supported = true;
    RemoteInputScriptState state = RemoteInputScriptState::Idle;
    QString runId;
    QString scriptName;
    QString scriptHash;
    int completedLoops = 0;
    int configuredLoops = 1;
    int eventIndex = 0;
    int eventCount = 0;
    qint64 startedAtEpochMs = 0;
    quint64 revision = 0;
    QString errorMessage;

    bool operator==(const RemoteInputScriptRuntimeInfo&) const = default;
}; // wjy: 被控端保存唯一权威快照，主控退出和重新连接都只读取这份状态，不依赖原 Viewer 会话。

struct RemoteInputScriptStartRequest {
    QString runId;
    QString fileName;
    qint64 fileSize = 0;
    QString sha256;
    int loopCount = 1;
    int loopIntervalMs = 0;
    double speedMultiplier = 1.0;
    bool pasteRandomSuffixEnabled = false;
    QString pasteRandomSeparator;
    int pasteRandomLength = 3;
    int pasteRandomMode = 0;
}; // wjy: F10 只传递共享文件标识和播放参数，脚本事件本身不再经过远控网络逐条发送。

enum class RemoteInputScriptCommandResult {
    Accepted,
    AlreadyRunning,
    NotRunning,
    RunIdMismatch,
    InvalidRequest,
    Failed,
};

QString remoteInputScriptSharedDirectory();
QString remoteInputScriptCacheDirectory();

class InputScriptExecutionService final : public QObject {
public:
    static InputScriptExecutionService& instance();

    RemoteInputScriptCommandResult start(
        const RemoteInputScriptStartRequest& request,
        QString* errorMessage = nullptr);
    RemoteInputScriptCommandResult stop(
        const QString& runId,
        QString* errorMessage = nullptr);
    RemoteInputScriptRuntimeInfo snapshot() const;
    void setStatusChangedCallback(std::function<void()> callback);
    void shutdown();

private:
    struct PreparationResult {
        bool success = false;
        bool cancelled = false;
        QString localFilePath;
        QString errorMessage;
        ui::RemoteInputScript script;
    };

    explicit InputScriptExecutionService(QObject* parent = nullptr);
    ~InputScriptExecutionService() override;
    InputScriptExecutionService(const InputScriptExecutionService&) = delete;
    InputScriptExecutionService& operator=(const InputScriptExecutionService&) = delete;

    static bool validStartRequest(const RemoteInputScriptStartRequest& request, QString* errorMessage);
    static PreparationResult prepareScript(
        const RemoteInputScriptStartRequest& request,
        const std::shared_ptr<std::atomic_bool>& cancelled);
    void finishPreparation(
        quint64 generation,
        const RemoteInputScriptStartRequest& request,
        PreparationResult result);
    void processDueEvents();
    void scheduleNextEvent();
    void finishLoop();
    bool injectEvent(const ui::RemoteInputEvent& event);
    void releaseHeldInputs();
    void resetPlaybackData();
    void setRuntimeState(RemoteInputScriptState state, const QString& errorMessage = QString());
    void updateRuntimeProgress();
    void publishStatusChanged();

    mutable QMutex m_statusMutex;
    RemoteInputScriptRuntimeInfo m_status;
    std::function<void()> m_statusChangedCallback;
    QTimer* m_playbackTimer = nullptr;
    QElapsedTimer m_playbackClock;
    QVector<ui::RemoteInputScriptEvent> m_events;
    qsizetype m_eventIndex = 0;
    int m_completedLoops = 0;
    RemoteInputScriptStartRequest m_request;
    QSet<int> m_heldKeys;
    QSet<int> m_heldButtons;
    int m_lastNormalizedX = 32768;
    int m_lastNormalizedY = 32768;
    bool m_ctrlDown = false;
    quint64 m_prepareGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_prepareCancelled;
    std::thread m_prepareThread;
    bool m_shuttingDown = false;
};
// ===end====

} // namespace platform
