#include "system/StartupPerformanceLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QThread>

#include <atomic>

namespace platform {
namespace {

// =====wjy====
constexpr const char* kStartupTimingLogFileName = "FSRemote_startup_timing.log";
constexpr const char* kStartupTimingSettingsFileName = "FSRemote_startup_timing.ini";
std::atomic_bool gStartupTimingCheckpointDisabled{false}; // wjy: 日志关闭或三秒观察结束后，所有检查点在加锁前直接返回。

struct StartupPerformanceLogState {
    QMutex mutex;
    QFile file;
    QElapsedTimer elapsed;
    QString logPath;
    QString settingsPath;
    qint64 lastCheckpointMs = 0;
    bool initialized = false;
    bool enabled = false;
    bool active = false;
};

StartupPerformanceLogState& startupPerformanceLogState()
{
    static StartupPerformanceLogState* state = new StartupPerformanceLogState; // wjy: 保持到进程结束，异常启动退出时已 flush 的步骤日志不会受静态析构顺序影响。
    return *state;
}

void writeRawLine(StartupPerformanceLogState& state, const QString& text)
{
    if (!state.file.isOpen()) {
        return;
    }
    state.file.write(text.toUtf8());
    state.file.write("\r\n");
    state.file.flush(); // wjy: 每一步立即落盘，程序卡死或被强制结束时仍能看到最后一个已完成检查点。
}

void initializeStartupPerformanceLog(StartupPerformanceLogState& state)
{
    if (state.initialized) {
        return;
    }
    state.initialized = true;
    state.elapsed.start();

    const QString executableDirectory = QCoreApplication::applicationDirPath();
    state.logPath = QDir(executableDirectory).filePath(QString::fromLatin1(kStartupTimingLogFileName));
    state.settingsPath = QDir(executableDirectory).filePath(QString::fromLatin1(kStartupTimingSettingsFileName));

    QSettings settings(state.settingsPath, QSettings::IniFormat);
    const QString enabledKey = QStringLiteral("StartupTiming/Enabled");
    if (!settings.contains(enabledKey)) {
        settings.setValue(enabledKey, true); // wjy: 首次发布默认开启诊断；用户把 INI 中 Enabled 改为 false 后下次启动立即关闭。
        settings.sync();
    }
    state.enabled = settings.value(enabledKey, true).toBool();
    if (!state.enabled) {
        gStartupTimingCheckpointDisabled.store(true, std::memory_order_release);
        return;
    }

    state.file.setFileName(state.logPath);
    if (!state.file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        gStartupTimingCheckpointDisabled.store(true, std::memory_order_release);
        return; // wjy: 可执行目录无写权限时静默放弃计时，绝不因诊断日志影响程序启动。
    }

    state.active = true;
    writeRawLine(state, QStringLiteral("==== FSRemote startup timing %1 ====")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    writeRawLine(state, QStringLiteral("executable=%1").arg(QCoreApplication::applicationFilePath()));
    writeRawLine(state, QStringLiteral("switch=%1").arg(state.settingsPath));
    writeRawLine(state, QStringLiteral("format: total_ms | step_ms | tid | step"));
    state.lastCheckpointMs = state.elapsed.elapsed(); // wjy: 表头写盘耗时不计入第一个真实启动步骤。
}

void writeCheckpointLocked(StartupPerformanceLogState& state, const QString& stepName)
{
    if (!state.active || stepName.isEmpty()) {
        return;
    }

    const qint64 totalMs = state.elapsed.elapsed();
    const qint64 stepMs = qMax<qint64>(0, totalMs - state.lastCheckpointMs);
    writeRawLine(state, QStringLiteral("%1 | %2 | %3 | %4")
        .arg(totalMs, 8)
        .arg(stepMs, 8)
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()))
        .arg(stepName)); // wjy: 固定列分别记录启动累计、本步骤耗时、线程 ID 和步骤名称，直接按 step_ms 找最慢项。
    state.lastCheckpointMs = state.elapsed.elapsed(); // wjy: 当前日志 flush 自身不计入下一业务步骤，减少诊断工具对结果的干扰。
}
// ===end====

} // namespace

// =====wjy====
void StartupPerformanceLog::checkpoint(const QString& stepName)
{
    if (gStartupTimingCheckpointDisabled.load(std::memory_order_acquire)) {
        return; // wjy: 运行期和 INI 关闭状态只执行一次原子布尔读取，不再进入互斥锁或文件系统。
    }
    StartupPerformanceLogState& state = startupPerformanceLogState();
    QMutexLocker locker(&state.mutex);
    initializeStartupPerformanceLog(state);
    writeCheckpointLocked(state, stepName);
}

void StartupPerformanceLog::finish(const QString& finalStepName)
{
    StartupPerformanceLogState& state = startupPerformanceLogState();
    QMutexLocker locker(&state.mutex);
    initializeStartupPerformanceLog(state);
    writeCheckpointLocked(state, finalStepName);
    if (state.active) {
        writeRawLine(state, QStringLiteral("==== startup timing finished ===="));
        state.active = false; // wjy: 观察窗口结束后所有现有诊断调用只走一次布尔判断，不再产生磁盘写入。
        state.file.close();
    }
    gStartupTimingCheckpointDisabled.store(true, std::memory_order_release); // wjy: 三秒后永久关闭本进程检查点热路径，下次启动再重新读取 INI。
}

QString StartupPerformanceLog::logFilePath()
{
    StartupPerformanceLogState& state = startupPerformanceLogState();
    QMutexLocker locker(&state.mutex);
    initializeStartupPerformanceLog(state);
    return state.logPath;
}

QString StartupPerformanceLog::settingsFilePath()
{
    StartupPerformanceLogState& state = startupPerformanceLogState();
    QMutexLocker locker(&state.mutex);
    initializeStartupPerformanceLog(state);
    return state.settingsPath;
}

bool StartupPerformanceLog::isEnabled()
{
    StartupPerformanceLogState& state = startupPerformanceLogState();
    QMutexLocker locker(&state.mutex);
    initializeStartupPerformanceLog(state);
    return state.enabled;
}
// ===end====

} // namespace platform
