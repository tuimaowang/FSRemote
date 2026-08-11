#include "system/WjyDiagnosticLog.h"
#include "system/RuntimeLogManager.h" // wjy: 诊断日志通过统一管理器固定写入当前安装目录data。

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

namespace platform {

namespace {

// =====wjy====
struct DiagnosticLogState {
    QMutex mutex;
    QFile file;
    QString path;
};

DiagnosticLogState& diagnosticLogState()
{
    static DiagnosticLogState* state = new DiagnosticLogState; // wjy: 故意保持到进程结束，避免静态析构顺序晚于后台线程最后一条退出日志。
    return *state;
}

bool ensureDiagnosticFileOpen(DiagnosticLogState& state)
{
    if (state.path.isEmpty()) {
        const QString dataDirectory = RuntimeLogManager::dataDirectory(); // wjy: 低频诊断不再写 AppData，统一跟随当前 FSRemote.exe 的 data 目录。
        if (dataDirectory.isEmpty() || !QDir().mkpath(dataDirectory)) {
            return false;
        }
        state.path = QDir(dataDirectory).filePath(QStringLiteral("fsremote_diagnostic.log"));
    }
    if (state.file.isOpen()) {
        return true;
    }
    state.file.setFileName(state.path);
    return state.file.open(QIODevice::WriteOnly | QIODevice::Append); // wjy: 单文件句柄复用，避免每条低频诊断反复打开关闭。
}

void rotateDiagnosticFileIfNeeded(DiagnosticLogState& state, qsizetype incomingBytes)
{
    constexpr qint64 kMaximumLogBytes = 4 * 1024 * 1024; // wjy: 20窗口长时间运行时日志总量最多约8MB（当前+单份备份）。
    if (!state.file.isOpen() || state.file.size() + incomingBytes <= kMaximumLogBytes) {
        return;
    }
    state.file.close();
    const QString backupPath = state.path + QStringLiteral(".1");
    QFile::remove(backupPath);
    QFile::rename(state.path, backupPath);
    state.file.setFileName(state.path);
    if (!state.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return; // wjy: 轮转后重新打开失败时静默丢弃本条诊断，绝不因日志问题影响远控主流程。
    }
}
// ===end====

} // namespace

void writeWjyDiagnosticLog(const QString& message)
{
    if (message.isEmpty()) {
        return;
    }
    // =====wjy====
    QByteArray line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8();
    line += " tid=";
    line += QByteArray::number(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    line += ' ';
    line += message.toUtf8();
    line += "\r\n";

    DiagnosticLogState& state = diagnosticLogState();
    QMutexLocker locker(&state.mutex); // wjy: Host、Viewer、Qt和生命周期线程可同时记录，整行写入不会交叉损坏。
    if (!ensureDiagnosticFileOpen(state)) {
        return;
    }
    rotateDiagnosticFileIfNeeded(state, line.size());
    if (state.file.isOpen()) {
        state.file.write(line);
        state.file.flush(); // wjy: 诊断频率受调用端限制为低频；每次落盘确保异常退出前的资源快照可用。
    }
    // ===end====
}

} // namespace platform
