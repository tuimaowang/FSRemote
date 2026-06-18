#include "system/ParsecVddInstaller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "parsec-vdd.h"

namespace platform {
namespace {

bool waitProcess(HANDLE processHandle, DWORD timeoutMs, DWORD* exitCode)
{
    if (!processHandle) {
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(processHandle, timeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        return false;
    }

    DWORD code = STILL_ACTIVE;
    if (!GetExitCodeProcess(processHandle, &code)) {
        return false;
    }
    if (exitCode) {
        *exitCode = code;
    }
    return true;
}

bool driverReadyStatus(parsec_vdd::DeviceStatus status)
{
    return status == parsec_vdd::DEVICE_OK;
}

bool driverInstalledStatus(parsec_vdd::DeviceStatus status)
{
    return status != parsec_vdd::DEVICE_NOT_INSTALLED;
}

QString lastWin32ErrorMessage()
{
    const DWORD error = GetLastError();
    if (error == 0) {
        return {};
    }

    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    QString message = length > 0 && buffer
        ? QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed()
        : QStringLiteral("Win32 error %1").arg(error);
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}

} // namespace

QString ParsecVddInstaller::installerPath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("parsec_vdd/parsec-vdd-0.45.0.0.exe"));
}

bool ParsecVddInstaller::isInstalled()
{
    const parsec_vdd::DeviceStatus status =
        parsec_vdd::QueryDeviceStatus(&parsec_vdd::VDD_CLASS_GUID, parsec_vdd::VDD_HARDWARE_ID);
    return driverInstalledStatus(status);
}

bool ParsecVddInstaller::ensureInstalled(QString* errorMessage)
{
    const parsec_vdd::DeviceStatus status =
        parsec_vdd::QueryDeviceStatus(&parsec_vdd::VDD_CLASS_GUID, parsec_vdd::VDD_HARDWARE_ID);
    if (driverReadyStatus(status) || driverInstalledStatus(status)) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    const QString path = installerPath();
    if (!QFileInfo::exists(path)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("找不到 parsec-vdd 安装包：%1").arg(path);
        }
        return false;
    }

    SHELLEXECUTEINFOW executeInfo = {};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.lpVerb = L"runas";
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    executeInfo.lpFile = nativePath.c_str();
    executeInfo.lpParameters = L"/S";
    executeInfo.nShow = SW_HIDE;

    if (!ShellExecuteExW(&executeInfo)) {
        const DWORD shellError = GetLastError();
        if (errorMessage) {
            *errorMessage = shellError == ERROR_CANCELLED
                ? QStringLiteral("已取消 parsec-vdd 驱动安装")
                : lastWin32ErrorMessage();
        }
        return false;
    }

    DWORD exitCode = 0;
    const bool finished = waitProcess(executeInfo.hProcess, 120000, &exitCode);
    CloseHandle(executeInfo.hProcess);
    if (!finished) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("等待 parsec-vdd 安装完成超时");
        }
        return false;
    }

    const parsec_vdd::DeviceStatus statusAfter =
        parsec_vdd::QueryDeviceStatus(&parsec_vdd::VDD_CLASS_GUID, parsec_vdd::VDD_HARDWARE_ID);
    if (driverInstalledStatus(statusAfter)) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("parsec-vdd 安装失败，退出码：%1").arg(exitCode);
    }
    return false;
}

} // namespace platform
