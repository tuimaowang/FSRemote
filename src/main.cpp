#include "ui/MainWindow.h"

#include "stream/StreamRuntime.h"
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceStatusService.h"
#include "system/ParsecVddInstaller.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"
#include "system/WjyDiagnosticLog.h"

#include <QApplication>
#include <QFont>

namespace {

void writeStartupLog(const QString& message)
{
    platform::writeWjyDiagnosticLog(message); // wjy: Keep startup diagnostics behind one helper so logging can be changed centrally.
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    writeStartupLog(QStringLiteral("[wjy-main] app created"));

    QApplication::setApplicationName(QStringLiteral("FSRemote"));
    QApplication::setOrganizationName(QStringLiteral("FSRemote"));
    writeStartupLog(QStringLiteral("[wjy-main] app metadata set"));

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(13);
    app.setFont(font);
    writeStartupLog(QStringLiteral("[wjy-main] font set"));

    writeStartupLog(QStringLiteral("[wjy-main] before ParsecVddInstaller::ensureInstalled")); // wjy: Restore virtual display driver preparation before stream host starts.
    platform::ParsecVddInstaller::ensureInstalled(); // wjy: Ensure the Parsec VDD driver exists; if missing, the installer may request elevation.
    writeStartupLog(QStringLiteral("[wjy-main] after ParsecVddInstaller::ensureInstalled")); // wjy: Continue even if the installer did not run, matching the previous non-blocking startup style.

    // =====wjy====
    FsRemoteHostConfig hostConfig = {}; // wjy: 在 Qt 层汇总可持久化配置，再通过稳定 C ABI 一次性复制到原生 DLL。
    hostConfig.struct_size = sizeof(hostConfig);
    hostConfig.version = 1;
    hostConfig.max_sessions = static_cast<uint32_t>(platform::AppSettings::remoteHostMaxSessions()); // wjy: 当前 DLL 仍强制有效会话数为 1，配置先为后续多会话开放做好接口。
    hostConfig.max_aggregate_video_kbps = static_cast<uint32_t>(platform::AppSettings::remoteHostAggregateVideoKbps());
    hostConfig.handshake_timeout_ms = static_cast<uint32_t>(platform::AppSettings::remoteHostHandshakeTimeoutMs());
    hostConfig.ownership_policy = FSREMOTE_OWNERSHIP_EXCLUSIVE; // wjy: 首版只允许一个远端持有键鼠控制权。
    writeStartupLog(QStringLiteral("[wjy-main] before StreamRuntime::startHost maxSessions=%1 aggregateKbps=%2 handshakeMs=%3")
        .arg(hostConfig.max_sessions)
        .arg(hostConfig.max_aggregate_video_kbps)
        .arg(hostConfig.handshake_timeout_ms));
    FsRemoteStreamHandle hostHandle = stream::StreamRuntime::instance().startHost(49100, hostConfig); // wjy: 配置入口缺失时 StreamRuntime 会自动回退到旧版单会话启动函数。
    // ===end====
    writeStartupLog(QStringLiteral("[wjy-main] after StreamRuntime::startHost handle=%1").arg(hostHandle ? 1 : 0)); // wjy: Record whether the native stream host returned a valid handle.

    platform::DeviceStatusServer statusServer([hostHandle] {
        return hostHandle && stream::StreamRuntime::instance().isBusy(hostHandle); // wjy: Report busy when the stream host is currently occupied.
    });
    writeStartupLog(QStringLiteral("[wjy-main] status server object created"));

    platform::DeviceCommandServer commandServer;
    writeStartupLog(QStringLiteral("[wjy-main] command server object created"));

    writeStartupLog(QStringLiteral("[wjy-main] before SSH server start")); // wjy: Start the bundled OpenSSH server for terminal/SFTP-related features.
    platform::PortableOpenSshManager::instance().startServer(); // wjy: Uses the openssh runtime under the application directory when present.
    writeStartupLog(QStringLiteral("[wjy-main] after SSH server start"));

    writeStartupLog(QStringLiteral("[wjy-main] before status server start")); // wjy: Start the online/busy status endpoint.
    statusServer.start(49101); // wjy: Port 49101 is queried by other FSRemote clients during refresh.
    writeStartupLog(QStringLiteral("[wjy-main] after status server start"));

    writeStartupLog(QStringLiteral("[wjy-main] before command server start")); // wjy: Start the remote command endpoint.
    commandServer.start(49102); // wjy: Port 49102 handles shutdown, restart, and wake proxy commands.
    writeStartupLog(QStringLiteral("[wjy-main] after command server start"));

    writeStartupLog(QStringLiteral("[wjy-main] before MainWindow create"));
    ui::MainWindow window;
    writeStartupLog(QStringLiteral("[wjy-main] after MainWindow create"));

    window.show();
    writeStartupLog(QStringLiteral("[wjy-main] window shown before app.exec"));

    const int result = app.exec();
    writeStartupLog(QStringLiteral("[wjy-main] app.exec returned"));

    platform::PowerManager::setPreventSleepEnabled(false);
    writeStartupLog(QStringLiteral("[wjy-main] prevent sleep disabled"));

    writeStartupLog(QStringLiteral("[wjy-main] before SSH server stop")); // wjy: Stop OpenSSH before application teardown so no sshd process is left behind.
    platform::PortableOpenSshManager::instance().stopServer(); // wjy: Explicit stop keeps shutdown order deterministic.
    writeStartupLog(QStringLiteral("[wjy-main] after SSH server stop"));

    writeStartupLog(QStringLiteral("[wjy-main] before command server stop")); // wjy: Stop the command TCP server before Qt object teardown.
    commandServer.stop(); // wjy: Releases the 49102 listener synchronously.
    writeStartupLog(QStringLiteral("[wjy-main] after command server stop"));

    writeStartupLog(QStringLiteral("[wjy-main] before status server stop")); // wjy: Stop the status TCP server before the stream host is released.
    statusServer.stop(); // wjy: Releases the 49101 listener synchronously.
    writeStartupLog(QStringLiteral("[wjy-main] after status server stop"));

    if (hostHandle) {
        stream::StreamRuntime::instance().stop(hostHandle); // wjy: Stop the native desktop stream host after all listeners are closed.
    }
    writeStartupLog(QStringLiteral("[wjy-main] stream host stopped"));

    return result;
}
