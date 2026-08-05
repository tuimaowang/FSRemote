#include "ui/RemoteConnectionState.h"

#include "FsRemoteStreamApi.h" // wjy: 状态测试与公开ABI常量保持同步，避免魔法数字漂移。

#include <cassert>

int main()
{
    // =====wjy====
    assert(ui::RemoteConnectionState::releasesViewerStartupAdmission(50)); // wjy: 首帧完成初始化后释放名额。
    assert(!ui::RemoteConnectionState::releasesViewerStartupAdmission(80)); // wjy: 断网需等原生Viewer停止完成后释放启动名额。
    assert(ui::RemoteConnectionState::releasesViewerStartupAdmission(90));
    assert(!ui::RemoteConnectionState::releasesViewerStartupAdmission(40));

    assert(ui::RemoteConnectionState::acceptsRemoteInput(50));
    assert(!ui::RemoteConnectionState::acceptsRemoteInput(40));
    assert(!ui::RemoteConnectionState::acceptsRemoteInput(80));
    assert(!ui::RemoteConnectionState::acceptsRemoteInput(FSREMOTE_STATUS_NETWORK_UNSTABLE));

    assert(ui::RemoteConnectionState::displayText(10) == QString::fromUtf8("正在连接 TCP"));
    assert(ui::RemoteConnectionState::displayText(50) == QString::fromUtf8("正在接收画面"));
    assert(ui::RemoteConnectionState::displayText(FSREMOTE_STATUS_NETWORK_UNSTABLE).contains(QString::fromUtf8("网络波动")));
    assert(ui::RemoteConnectionState::displayText(FSREMOTE_STATUS_NETWORK_RECOVERING).contains(QString::fromUtf8("重新连接")));
    assert(ui::RemoteConnectionState::displayText(90).contains(QString::fromUtf8("连接失败")));
    assert(ui::RemoteConnectionState::displayText(90, QStringLiteral("timeout")) == QString::fromUtf8("连接失败：timeout"));
    assert(ui::RemoteConnectionState::displayText(999, QStringLiteral("native detail")) == QStringLiteral("native detail")); // wjy: 未知原生状态保留上游提示，兼容后续协议扩展。
    // ===end====
    return 0;
}
