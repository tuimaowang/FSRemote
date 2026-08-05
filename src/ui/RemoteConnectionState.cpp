#include "ui/RemoteConnectionState.h"

#include "FsRemoteStreamApi.h" // wjy: 网络波动和恢复状态使用公开ABI常量，Qt映射不再保留魔法数字。

namespace ui {

// =====wjy====
bool RemoteConnectionState::releasesViewerStartupAdmission(int statusCode)
{
    return statusCode == 50 || statusCode == 90; // wjy: 断开必须等原生stop完成后再释放名额，避免旧Viewer与新Viewer并行初始化。
}

bool RemoteConnectionState::acceptsRemoteInput(int statusCode)
{
    return statusCode == 50; // wjy: 只有已收到首帧的连接才允许成为同步输入主控或跟随端。
}

QString RemoteConnectionState::displayText(int statusCode, const QString& detailMessage)
{
    switch (statusCode) {
    case 10:
        return QString::fromUtf8("正在连接 TCP");
    case 20:
        return QString::fromUtf8("TCP 已连接");
    case 30:
        return QString::fromUtf8("正在初始化 WebRTC");
    case 40:
        return QString::fromUtf8("等待远程画面");
    case 50:
        return QString::fromUtf8("正在接收画面");
    case FSREMOTE_STATUS_REMOTE_CLOSED:
        return QString::fromUtf8("连接已断开");
    case FSREMOTE_STATUS_NETWORK_UNSTABLE:
        return QString::fromUtf8("网络波动，正在等待恢复");
    case FSREMOTE_STATUS_NETWORK_RECOVERING:
        return detailMessage.isEmpty()
            ? QString::fromUtf8("正在重新连接")
            : detailMessage;
    case FSREMOTE_STATUS_ERROR:
        return detailMessage.isEmpty()
            ? QString::fromUtf8("连接失败")
            : QString::fromUtf8("连接失败：%1").arg(detailMessage); // wjy: 失败详情只负责显示，不参与连接状态判断或生命周期分支。
    default:
        return detailMessage.isEmpty() ? QString::fromUtf8("正在连接") : detailMessage; // wjy: 未知状态继续兼容原生层传入的可读提示。
    }
}
// ===end====

} // namespace ui
