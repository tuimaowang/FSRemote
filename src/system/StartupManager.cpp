#include "system/StartupManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace platform {
namespace {

constexpr const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const char* kValueName = "FSRemote";

QString startupCommand()
{
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}

QSettings runSettings()
{
    return QSettings(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
}

} // namespace

bool StartupManager::isEnabled()
{
    QSettings settings = runSettings();
    return settings.value(QString::fromLatin1(kValueName)).toString() == startupCommand();
}

bool StartupManager::setEnabled(bool enabled)
{
    QSettings settings = runSettings();
    if (enabled) {
        settings.setValue(QString::fromLatin1(kValueName), startupCommand());
    } else {
        settings.remove(QString::fromLatin1(kValueName));
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace platform
