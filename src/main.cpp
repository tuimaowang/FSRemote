#include "ui/MainWindow.h"
#include "stream/StreamRuntime.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceStatusService.h"
#include "system/ParsecVddInstaller.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"

#include <QApplication>
#include <QFont>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FSRemote"));
    QApplication::setOrganizationName(QStringLiteral("FSRemote"));

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(13);
    app.setFont(font);

    platform::ParsecVddInstaller::ensureInstalled();

    FsRemoteStreamHandle hostHandle = stream::StreamRuntime::instance().startHost(49100);
    platform::DeviceStatusServer statusServer([hostHandle] {
        return stream::StreamRuntime::instance().isBusy(hostHandle);
    });
    platform::DeviceCommandServer commandServer;
    platform::PortableOpenSshManager::instance().startServer();
    statusServer.start(49101);
    commandServer.start(49102);
    ui::MainWindow window;
    window.show();
    const int result = app.exec();
    platform::PowerManager::setPreventSleepEnabled(false);
    platform::PortableOpenSshManager::instance().stopServer();
    commandServer.stop();
    statusServer.stop();
    stream::StreamRuntime::instance().stop(hostHandle);
    return result;
}
