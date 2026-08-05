#pragma once

// =====wjy====
#include "faker_input_bridge_client.h"

#include <Windows.h>
#include <wincrypt.h>

#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace uu {
namespace faker_input_runtime_detail {

using faker_input_bridge_detail::windowsErrorText; // wjy: 复用 Bridge 客户端统一的 Win32 数字错误格式，诊断日志保持可搜索且不受系统语言影响。

constexpr wchar_t kBridgeFileName[] = L"FakerInputBridge.exe"; // wjy: Bridge 与 FSRemote 放在同一发布目录，目标端无需手工填写路径。
constexpr wchar_t kInstallerFileName[] = L"FakerInput_Setup_0.1.1_x64.msi"; // wjy: 运行时只接受已经验证过的 0.1.1 x64 安装包文件名。
constexpr wchar_t kProductCode[] = L"{17AA3E01-1012-4BF7-B908-1C499909B259}"; // wjy: 使用 MSI ProductCode 判断当前版本是否已经登记，避免每次启动都执行维护安装。
constexpr std::string_view kBridgeSha256 = "440FADF4D09000AE3BFEF115DA45A3D2F5F90C4FC4E124729C837FB47C628192"; // wjy: 固定当前独立 Bridge 成品哈希，拒绝同目录被替换的未知程序。
constexpr std::string_view kInstallerSha256 = "4C0AEFB7340051A91D606776243298B5CD1143EF5508BBAE6800C474F9ED0840"; // wjy: 在静默提权安装前锁定已验签 MSI 的确切内容，防止用户可写目录成为提权入口。
constexpr std::string_view kPublisherThumbprint = "B8104824B17AB43A41350E6A35D1FB52B47DC2D7"; // wjy: 只允许信任当前 FakerInput MSI 与驱动目录共同使用的 Ryodigi Solutions LLC 签名证书。

inline std::wstring executableDirectory(std::string* error)
{
    std::vector<wchar_t> path(32768, L'\0'); // wjy: 使用 Windows 最大长路径缓冲区，中文或深层安装目录不会被 MAX_PATH 截断。
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        if (error) *error = windowsErrorText("locate FSRemote executable", ::GetLastError());
        return {};
    }
    const std::wstring executablePath(path.data(), length);
    const std::size_t separator = executablePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        if (error) *error = "FSRemote executable directory is invalid";
        return {};
    }
    return executablePath.substr(0, separator); // wjy: 所有运行资源均从主程序真实目录解析，不依赖可被远端环境改变的当前工作目录。
}

inline std::wstring childPath(const std::wstring& directory, const wchar_t* fileName)
{
    return directory + L"\\" + fileName;
}

inline bool regularFileExists(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline int hexNibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

inline bool sha256Matches(const std::wstring& path, std::string_view expectedHex, std::string* error)
{
    if (expectedHex.size() != 64) {
        if (error) *error = "invalid pinned SHA-256 length";
        return false;
    }

    const HANDLE file = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = windowsErrorText("open FakerInput runtime file", ::GetLastError());
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool success = ::CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE
        && ::CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    DWORD failure = success ? ERROR_SUCCESS : ::GetLastError();
    std::array<BYTE, 64 * 1024> buffer{};
    while (success) {
        DWORD read = 0;
        if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            failure = ::GetLastError();
            success = false;
            break;
        }
        if (read == 0) break;
        if (!::CryptHashData(hash, buffer.data(), read, 0)) {
            failure = ::GetLastError();
            success = false;
        }
    }

    std::array<BYTE, 32> actual{};
    DWORD actualBytes = static_cast<DWORD>(actual.size());
    if (success && !::CryptGetHashParam(hash, HP_HASHVAL, actual.data(), &actualBytes, 0)) {
        failure = ::GetLastError();
        success = false;
    }
    if (hash) ::CryptDestroyHash(hash);
    if (provider) ::CryptReleaseContext(provider, 0);
    ::CloseHandle(file);
    if (!success || actualBytes != actual.size()) {
        if (error) *error = windowsErrorText("hash FakerInput runtime file", failure);
        return false;
    }

    for (std::size_t index = 0; index < actual.size(); ++index) {
        const int high = hexNibble(expectedHex[index * 2]);
        const int low = hexNibble(expectedHex[index * 2 + 1]);
        if (high < 0 || low < 0 || actual[index] != static_cast<BYTE>((high << 4) | low)) {
            if (error) *error = "FakerInput runtime SHA-256 mismatch";
            return false; // wjy: 哈希不一致时绝不启动 Bridge 或把 MSI 交给管理员令牌执行。
        }
    }
    return true;
}

inline bool currentProcessElevated(std::string* error)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (error) *error = windowsErrorText("open FSRemote process token", ::GetLastError());
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool success = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE;
    const DWORD failure = success ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(token);
    if (!success) {
        if (error) *error = windowsErrorText("read FSRemote elevation state", failure);
        return false;
    }
    if (elevation.TokenIsElevated == 0) {
        if (error) *error = "FakerInput installation requires an elevated FSRemote process";
        return false; // wjy: 静默参数不能绕过 UAC；无管理员令牌时立即安全失败，避免被控端停在不可见的安全桌面提示上。
    }
    return true;
}

inline bool productInstalled(bool* querySucceeded, std::string* error)
{
    if (querySucceeded) *querySucceeded = false;
    wchar_t systemDirectory[32768] = {};
    const UINT length = ::GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (length == 0 || length >= std::size(systemDirectory)) {
        if (error) *error = windowsErrorText("locate Windows system directory", ::GetLastError());
        return false;
    }
    const std::wstring msiLibrary = std::wstring(systemDirectory, length) + L"\\msi.dll";
    const HMODULE module = ::LoadLibraryW(msiLibrary.c_str());
    if (!module) {
        if (error) *error = windowsErrorText("load Windows Installer API", ::GetLastError());
        return false;
    }
    using QueryProductState = int(WINAPI*)(LPCWSTR);
    const auto query = reinterpret_cast<QueryProductState>(::GetProcAddress(module, "MsiQueryProductStateW"));
    if (!query) {
        const DWORD failure = ::GetLastError();
        ::FreeLibrary(module);
        if (error) *error = windowsErrorText("resolve Windows Installer API", failure);
        return false;
    }
    const int state = query(kProductCode);
    ::FreeLibrary(module);
    if (querySucceeded) *querySucceeded = true;
    return state == 5; // wjy: 只有 INSTALLSTATE_DEFAULT 表示产品已完整安装；仅 advertised 的设备仍必须执行 MSI 才能获得驱动。
}

inline bool runHiddenAndWait(
    const std::wstring& executable,
    const std::wstring& commandLine,
    const std::wstring& workingDirectory,
    DWORD timeoutMs,
    DWORD* exitCode,
    std::string* error)
{
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(
            executable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            workingDirectory.c_str(),
            &startup,
            &process)) {
        if (error) *error = windowsErrorText("start silent FakerInput installer", ::GetLastError());
        return false;
    }
    ::CloseHandle(process.hThread);
    const DWORD wait = ::WaitForSingleObject(process.hProcess, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        const DWORD failure = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError();
        ::CloseHandle(process.hProcess); // wjy: 超时只停止等待而不强杀 msiexec，避免在驱动提交阶段破坏 Windows Installer 事务。
        if (error) *error = windowsErrorText("wait silent FakerInput installer", failure);
        return false;
    }
    DWORD result = ERROR_GEN_FAILURE;
    const bool queried = ::GetExitCodeProcess(process.hProcess, &result) != FALSE;
    const DWORD failure = queried ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(process.hProcess);
    if (!queried) {
        if (error) *error = windowsErrorText("read silent FakerInput installer result", failure);
        return false;
    }
    if (exitCode) *exitCode = result;
    return true;
}

inline bool trustPinnedInstallerPublisher(const std::wstring& installerPath, std::string* error)
{
    wchar_t systemDirectory[32768] = {}; // wjy: 从受 Windows 保护的 System32 解析 msi.dll，禁止从程序目录加载同名库。
    const UINT systemLength = ::GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (systemLength == 0 || systemLength >= std::size(systemDirectory)) {
        if (error) *error = windowsErrorText("locate Windows system directory for MSI signature", ::GetLastError());
        return false;
    }

    const std::wstring msiLibrary = std::wstring(systemDirectory, systemLength) + L"\\msi.dll";
    const HMODULE module = ::LoadLibraryW(msiLibrary.c_str());
    if (!module) {
        if (error) *error = windowsErrorText("load Windows Installer signature API", ::GetLastError());
        return false;
    }

    using GetFileSignatureInformation = HRESULT(WINAPI*)(LPCWSTR, DWORD, PCCERT_CONTEXT*, BYTE*, DWORD*);
    const auto getSignature = reinterpret_cast<GetFileSignatureInformation>(
        ::GetProcAddress(module, "MsiGetFileSignatureInformationW"));
    if (!getSignature) {
        const DWORD failure = ::GetLastError();
        ::FreeLibrary(module);
        if (error) *error = windowsErrorText("resolve Windows Installer signature API", failure);
        return false;
    }

    PCCERT_CONTEXT signer = nullptr;
    constexpr DWORD kMsiInvalidHashIsFatal = 0x1; // wjy: 对应 MSI_INVALID_HASH_IS_FATAL；除固定文件哈希外还要求 Authenticode 内部摘要本身有效。
    const HRESULT signatureResult = getSignature(
        installerPath.c_str(), kMsiInvalidHashIsFatal, &signer, nullptr, nullptr); // wjy: 证书直接从已经固定 SHA-256 的 MSI 提取，旧客户端首次升级无需预先认识新增 .cer 文件。
    ::FreeLibrary(module);
    if (signatureResult != S_OK || !signer) {
        if (error) *error = "read FakerInput MSI signer failed code=" + std::to_string(static_cast<long>(signatureResult));
        return false;
    }

    std::array<BYTE, 20> actualThumbprint{};
    DWORD thumbprintBytes = static_cast<DWORD>(actualThumbprint.size());
    if (!::CertGetCertificateContextProperty(
            signer, CERT_SHA1_HASH_PROP_ID, actualThumbprint.data(), &thumbprintBytes)
        || thumbprintBytes != actualThumbprint.size()) {
        const DWORD failure = ::GetLastError();
        ::CertFreeCertificateContext(signer);
        if (error) *error = windowsErrorText("read FakerInput publisher thumbprint", failure);
        return false;
    }
    for (std::size_t index = 0; index < actualThumbprint.size(); ++index) {
        const int high = hexNibble(kPublisherThumbprint[index * 2]);
        const int low = hexNibble(kPublisherThumbprint[index * 2 + 1]);
        if (high < 0 || low < 0
            || actualThumbprint[index] != static_cast<BYTE>((high << 4) | low)) {
            ::CertFreeCertificateContext(signer);
            if (error) *error = "FakerInput MSI publisher certificate mismatch";
            return false; // wjy: MSI 内容哈希和发布者指纹必须同时匹配，不能把被替换的任意签名证书加入系统信任区。
        }
    }

    const HCERTSTORE trustedPublishers = ::CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
        L"TrustedPublisher"); // wjy: 驱动安装由管理员进程执行，因此写入本机可信发布者，覆盖所有 Windows 服务使用的设备安装上下文。
    if (!trustedPublishers) {
        const DWORD failure = ::GetLastError();
        ::CertFreeCertificateContext(signer);
        if (error) *error = windowsErrorText("open LocalMachine TrustedPublisher store", failure);
        return false;
    }

    const bool trusted = ::CertAddCertificateContextToStore(
        trustedPublishers, signer, CERT_STORE_ADD_USE_EXISTING, nullptr) != FALSE; // wjy: 已存在时保持原证书属性并视为成功，新设备则只加入经过双重固定校验的同一张证书。
    const DWORD trustFailure = trusted ? ERROR_SUCCESS : ::GetLastError();
    ::CertCloseStore(trustedPublishers, 0);
    ::CertFreeCertificateContext(signer);
    if (!trusted) {
        if (error) *error = windowsErrorText("trust FakerInput publisher certificate", trustFailure);
        return false;
    }
    return true;
}

} // namespace faker_input_runtime_detail

class FakerInputRuntimeProvisioner final {
public:
    ~FakerInputRuntimeProvisioner()
    {
        stopOwnedBridge(); // wjy: FSRemote 正常退出或 DLL 卸载时关闭自己启动的 Bridge，更新程序不会被旧进程锁住文件。
    }

    FakerInputRuntimeProvisioner() = default;
    FakerInputRuntimeProvisioner(const FakerInputRuntimeProvisioner&) = delete;
    FakerInputRuntimeProvisioner& operator=(const FakerInputRuntimeProvisioner&) = delete;

    bool ensureReady(std::string* error)
    {
        using namespace faker_input_runtime_detail;
        std::string probeError;
        if (probeBridge(100, &probeError)) return true; // wjy: 优先复用用户或另一组件已经启动的本机 Bridge，绝不重复安装或创建第二个服务进程。

        const std::wstring directory = executableDirectory(error);
        if (directory.empty()) return false;
        const std::wstring bridgePath = childPath(directory, kBridgeFileName);
        const std::wstring installerPath = childPath(directory, kInstallerFileName);
        if (!regularFileExists(bridgePath)) {
            if (error) *error = "FakerInputBridge.exe is missing beside FSRemote.exe";
            return false;
        }

        std::string bridgeError;
        if (startOwnedBridge(bridgePath, directory, &bridgeError)
            && probeBridge(5000, &bridgeError)) {
            return true; // wjy: 驱动已存在时只隐藏启动 Bridge，整个路径不会触发 MSI 或管理员检查。
        }
        stopOwnedBridge();

        bool querySucceeded = false;
        std::string queryError;
        if (productInstalled(&querySucceeded, &queryError)) {
            if (error) *error = "FakerInput is installed but its bridge could not start: " + bridgeError;
            return false; // wjy: 已登记产品启动失败时不盲目反复维护安装，保留明确故障供日志诊断。
        }
        if (!regularFileExists(installerPath)) {
            if (error) *error = "FakerInput_Setup_0.1.1_x64.msi is missing beside FSRemote.exe";
            return false;
        }
        if (!sha256Matches(installerPath, kInstallerSha256, error)) return false;
        if (!currentProcessElevated(error)) return false;
        if (!trustPinnedInstallerPublisher(installerPath, error)) return false; // wjy: 先信任 MSI 内固定签名证书，再进入驱动安装，避免无人值守设备弹出无法远控点击的 Windows 安全确认。

        wchar_t systemDirectory[32768] = {};
        const UINT systemLength = ::GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
        if (systemLength == 0 || systemLength >= std::size(systemDirectory)) {
            if (error) *error = windowsErrorText("locate msiexec.exe", ::GetLastError());
            return false;
        }
        const std::wstring msiexec = std::wstring(systemDirectory, systemLength) + L"\\msiexec.exe";
        const std::wstring command = L"\"" + msiexec + L"\" /i \"" + installerPath + L"\" /qn /norestart";
        DWORD installerExitCode = ERROR_GEN_FAILURE;
        if (!runHiddenAndWait(msiexec, command, directory, 10 * 60 * 1000, &installerExitCode, error)) return false;
        const bool restartRequired = installerExitCode == ERROR_SUCCESS_REBOOT_REQUIRED
            || installerExitCode == ERROR_SUCCESS_REBOOT_INITIATED;
        if (installerExitCode != ERROR_SUCCESS && !restartRequired) {
            if (error) *error = "silent FakerInput installer exited with code " + std::to_string(installerExitCode);
            return false;
        }

        ::Sleep(500); // wjy: 给 PnP 完成根设备枚举的短暂窗口，再启动只在进程初始化时打开驱动句柄的 Bridge。
        if (!startOwnedBridge(bridgePath, directory, &bridgeError)
            || !probeBridge(15000, &bridgeError)) {
            stopOwnedBridge();
            if (error) {
                *error = restartRequired
                    ? "FakerInput installed successfully and requires a Windows restart"
                    : "FakerInput installed but bridge readiness failed: " + bridgeError;
            }
            return false;
        }
        return true; // wjy: 只有 MSI 成功且新 Bridge 完成 driver-ready ping 后，Host 才允许切换为驱动鼠标。
    }

private:
    bool probeBridge(DWORD timeoutMs, std::string* error)
    {
        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        std::string latestError;
        do {
            FakerInputBridgeClient probe;
            if (probe.connectAndPing(&latestError)) return true;
            if (ownedBridgeProcess_ && ::WaitForSingleObject(ownedBridgeProcess_, 0) == WAIT_OBJECT_0) {
                DWORD exitCode = ERROR_GEN_FAILURE;
                ::GetExitCodeProcess(ownedBridgeProcess_, &exitCode);
                latestError = "FakerInputBridge exited with code " + std::to_string(exitCode);
                break;
            }
            if (::GetTickCount64() >= deadline) break;
            ::Sleep(100);
        } while (true);
        if (error) *error = latestError.empty() ? "FakerInputBridge readiness timed out" : latestError;
        return false;
    }

    bool startOwnedBridge(const std::wstring& bridgePath, const std::wstring& directory, std::string* error)
    {
        using namespace faker_input_runtime_detail;
        if (!sha256Matches(bridgePath, kBridgeSha256, error)) return false;
        stopOwnedBridge();

        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                ::CloseHandle(job);
                job = nullptr; // wjy: Job 创建失败不阻止输入功能，仍保留进程句柄以便正常退出时显式清理。
            }
        }

        std::wstring command = L"\"" + bridgePath + L"\" --server";
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(
                bridgePath.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                nullptr,
                directory.c_str(),
                &startup,
                &process)) {
            const DWORD failure = ::GetLastError();
            if (job) ::CloseHandle(job);
            if (error) *error = windowsErrorText("start FakerInputBridge", failure);
            return false;
        }
        if (job && !::AssignProcessToJobObject(job, process.hProcess)) {
            ::CloseHandle(job);
            job = nullptr; // wjy: 进程可能已继承外层更新 Job；嵌套失败时改用持有的进程句柄做正常退出清理。
        }
        if (::ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            const DWORD failure = ::GetLastError();
            ::TerminateProcess(process.hProcess, failure);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            if (job) ::CloseHandle(job);
            if (error) *error = windowsErrorText("resume FakerInputBridge", failure);
            return false;
        }
        ::CloseHandle(process.hThread);
        ownedBridgeProcess_ = process.hProcess;
        ownedBridgeJob_ = job;
        return true; // wjy: Bridge 不创建控制台窗口，只开放其自身限制为本机用户的命名管道。
    }

    void stopOwnedBridge() noexcept
    {
        if (!ownedBridgeProcess_) return;
        if (ownedBridgeJob_) {
            ::CloseHandle(ownedBridgeJob_); // wjy: 关闭 KILL_ON_JOB_CLOSE Job 可覆盖崩溃/更新退出，避免 Bridge 长期占用发布目录。
            ownedBridgeJob_ = nullptr;
        } else if (::WaitForSingleObject(ownedBridgeProcess_, 0) == WAIT_TIMEOUT) {
            ::TerminateProcess(ownedBridgeProcess_, ERROR_PROCESS_ABORTED); // wjy: 无法加入 Job 的兼容路径只终止本对象亲自创建的进程，不触碰用户手工启动的 Bridge。
        }
        ::WaitForSingleObject(ownedBridgeProcess_, 2000);
        ::CloseHandle(ownedBridgeProcess_);
        ownedBridgeProcess_ = nullptr;
    }

    HANDLE ownedBridgeJob_ = nullptr;
    HANDLE ownedBridgeProcess_ = nullptr;
};

} // namespace uu
// ===end====
