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
    // =====wjy====
    // wjy: 开机自启默认带 --minimized，进入托盘而不是直接弹主窗口。
    return QStringLiteral("\"%1\" --minimized")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    // ===end====
}

QSettings runSettings()
{
    return QSettings(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
}

} // namespace

bool StartupManager::isEnabled()
{
    QSettings settings = runSettings();
    const QString value = settings.value(QString::fromLatin1(kValueName)).toString();
    // =====wjy====
    // wjy: 兼容旧版未带 --minimized 的自启项，只要指向当前 exe 即视为已启用。
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return value == startupCommand() || value.contains(exePath, Qt::CaseInsensitive);
    // ===end====
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
