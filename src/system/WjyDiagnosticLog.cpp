#include "system/WjyDiagnosticLog.h"

#include <QByteArray>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <mutex>
#include <string>

namespace {

#if defined(Q_OS_WIN)
std::wstring diagnosticLogPath()
{
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0 || length >= std::size(localAppData)) {
        return L"fsremote_diagnostic.log"; // wjy: 极端情况下环境变量不可用，退回当前目录仍尽量留下退出证据。
    }

    std::wstring vendorDir(localAppData, length);
    vendorDir += L"\\FSRemote";
    CreateDirectoryW(vendorDir.c_str(), nullptr); // wjy: 目录已存在时 ERROR_ALREADY_EXISTS 可安全忽略。
    std::wstring appDir = vendorDir + L"\\FSRemote";
    CreateDirectoryW(appDir.c_str(), nullptr);
    return appDir + L"\\fsremote_diagnostic.log";
}
#endif

std::mutex g_diagnosticLogMutex; // wjy: 状态探测线程、UI 线程和退出看门狗可能同时写日志，统一串行化为完整行。

} // namespace

namespace platform {

void writeWjyDiagnosticLog(const QString& message)
{
#if defined(Q_OS_WIN)
    std::lock_guard lock(g_diagnosticLogMutex);
    const std::wstring path = diagnosticLogPath();
    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr); // wjy: 直接使用 Win32 追加并立即落盘，进程被 TerminateProcess 时最后一条打点仍能保留。
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char prefix[128]{};
    const int prefixLength = std::snprintf(
        prefix,
        sizeof(prefix),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu ",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId())); // wjy: 时间、PID 和线程 ID 用于区分更新前后两个进程及具体卡住线程。
    QByteArray line(prefix, prefixLength > 0 ? prefixLength : 0);
    line += message.toUtf8();
    line += "\r\n";
    DWORD written = 0;
    WriteFile(file, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr);
    FlushFileBuffers(file); // wjy: 退出诊断优先可靠性，每条日志强制刷新，防止强杀时仍停留在缓存。
    CloseHandle(file);
#else
    (void)message;
#endif
}

} // namespace platform
