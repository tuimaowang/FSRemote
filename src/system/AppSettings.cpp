#include "system/AppSettings.h"

#include <QSettings>

namespace platform {
namespace {

QSettings settings()
{
    return QSettings(QStringLiteral("FSRemote"), QStringLiteral("FSRemote"));
}

} // namespace

bool AppSettings::preventSleepEnabled()
{
    return settings().value(QStringLiteral("preventSleepEnabled"), true).toBool();
}

void AppSettings::setPreventSleepEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("preventSleepEnabled"), enabled);
}

bool AppSettings::remoteWakeupEnabled()
{
    return settings().value(QStringLiteral("remoteWakeupEnabled"), true).toBool(); //允许远程控制
}

void AppSettings::setRemoteWakeupEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteWakeupEnabled"), enabled);
}

bool AppSettings::statusAutoRefreshEnabled()
{
    return settings().value(QStringLiteral("statusAutoRefreshEnabled"), true).toBool();
}

void AppSettings::setStatusAutoRefreshEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("statusAutoRefreshEnabled"), enabled);
}

int AppSettings::statusAutoRefreshIntervalSeconds()
{
    const int seconds = settings().value(QStringLiteral("statusAutoRefreshIntervalSeconds"), 60).toInt();
    return seconds > 0 ? seconds : 60;
}

void AppSettings::setStatusAutoRefreshIntervalSeconds(int seconds)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("statusAutoRefreshIntervalSeconds"), seconds > 0 ? seconds : 60);
}

} // namespace platform
