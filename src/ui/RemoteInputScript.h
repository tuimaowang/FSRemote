#pragma once

#include "ui/RemoteInputBroadcastCoordinator.h"

#include <QSize>
#include <QString>
#include <QVector>

namespace ui {

// =====wjy====
struct RemoteInputScriptEvent {
    qint64 elapsedMs = 0; // wjy: 保存从按下F9开始到本事件发生的单调时间，回放不依赖系统时间或录制当天日期。
    RemoteInputEvent input; // wjy: 直接保存远控现有语义输入，绝对坐标已经归一化，窗口缩放后仍能落在同一远端位置。
};

struct RemoteInputScript {
    QString name;
    QString sourceHost;
    QSize sourceFrameSize;
    QVector<RemoteInputScriptEvent> events;
};

qint64 remoteInputScriptPlaybackTimeMs(
    qint64 recordedElapsedMs,
    double speedMultiplier); // wjy: 统一把录制时间换算为当前速度下的目标时间，速度2倍表示等待时间减半。
bool remoteInputScriptShouldRepeat(
    int configuredLoopCount,
    int completedLoopCount); // wjy: 0固定表示无限循环，正数按已完成的整轮数量判断是否继续。

class RemoteInputScriptStore final {
public:
    static QString defaultDirectory(); // wjy: 正式运行固定落到程序目录下的script文件夹，便于复制、备份和人工选择。
    static QString fileDialogFilter();
    static QString safeBaseName(const QString& requestedName);
    static bool saveToDirectory(
        const QString& directoryPath,
        const RemoteInputScript& script,
        QString* savedFilePath,
        QString* errorMessage); // wjy: 使用原子保存和自动避让重名文件，录制结束不会覆盖已有脚本。
    static bool loadFromFile(
        const QString& filePath,
        RemoteInputScript* script,
        QString* errorMessage); // wjy: 回放前完整校验版本、事件数量、时间顺序和输入字段，畸形本地文件不会进入远控发送路径。
};
// ===end====

} // namespace ui
