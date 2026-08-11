// =====wjy====
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct UpdateFile {
    fs::path relativePath; // wjy: 更新器只接收相对路径，阻止任务越界修改安装目录之外的文件。
    std::uintmax_t size = 0; // wjy: 保存主程序暂存时记录的文件大小，安装后再次核对复制结果。
};

struct UpdateTask {
    DWORD processId = 0; // wjy: 更新器等待这个主进程完全退出后才开始替换被占用的 EXE/DLL。
    fs::path sourceDir;
    fs::path targetDir;
    fs::path backupDir;
    fs::path restartExecutable;
    std::wstring fromVersion;
    std::wstring toVersion;
    std::vector<UpdateFile> files;
};

std::wofstream g_log;

void logLine(const std::wstring& text)
{
    if (g_log.is_open()) {
        SYSTEMTIME now{};
        GetLocalTime(&now); // wjy: 独立更新器不依赖 Qt，直接使用 Win32 本地时间为每个阶段补充毫秒时间戳。
        g_log << std::setfill(L'0')
              << std::setw(4) << now.wYear << L'-'
              << std::setw(2) << now.wMonth << L'-'
              << std::setw(2) << now.wDay << L' '
              << std::setw(2) << now.wHour << L':'
              << std::setw(2) << now.wMinute << L':'
              << std::setw(2) << now.wSecond << L'.'
              << std::setw(3) << now.wMilliseconds << L' '
              << text << L'\n'; // wjy: 时间戳可直接证明 60 秒花在等待主进程，而不是 Bridge 清理、文件安装或重启。
        g_log.flush();
    }
}

std::string readUtf8File(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string unescapeJsonString(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            result.push_back(value[i]);
            continue;
        }
        const char escaped = value[++i];
        switch (escaped) {
        case '\\': result.push_back('\\'); break;
        case '"': result.push_back('"'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        default: return {}; // wjy: 不接受未知转义，避免正则截取后的畸形路径被静默用于文件替换。
        }
    }
    return result;
}

std::string jsonString(const std::string& json, const char* key)
{
    const std::regex pattern(std::string("\"") + key + R"json("\s*:\s*"((?:\\.|[^"\\])*)")json"); // wjy: 捕获普通字符或完整转义序列，Windows 路径不会在第一个反斜杠处被截成 C:。
    std::smatch match;
    return std::regex_search(json, match, pattern) ? unescapeJsonString(match[1].str()) : std::string(); // wjy: 将 JSON 的双反斜杠还原成真实 Windows 路径，不依赖系统容忍重复分隔符。
}

long long jsonInteger(const std::string& json, const char* key, long long fallback = -1)
{
    const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    return std::regex_search(json, match, pattern) ? std::stoll(match[1].str()) : fallback;
}

bool isSafeRelativePath(const fs::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == L"..") return false; // wjy: 禁止父目录跳转，确保清单无法逃逸 source/target/backup 根目录。
    }
    return true;
}

bool parseTask(const fs::path& taskPath, UpdateTask* task, std::wstring* error)
{
    const std::string json = readUtf8File(taskPath);
    if (json.empty() || jsonInteger(json, "schemaVersion") != 1) {
        *error = L"unsupported or empty update task";
        return false;
    }
    task->processId = static_cast<DWORD>(jsonInteger(json, "processId", 0));
    task->sourceDir = utf8ToWide(jsonString(json, "sourceDir"));
    task->targetDir = utf8ToWide(jsonString(json, "targetDir"));
    task->backupDir = utf8ToWide(jsonString(json, "backupDir"));
    task->restartExecutable = utf8ToWide(jsonString(json, "restartExecutable"));
    task->fromVersion = utf8ToWide(jsonString(json, "fromVersion"));
    task->toVersion = utf8ToWide(jsonString(json, "toVersion"));
    if (task->processId == 0 || task->sourceDir.empty() || task->targetDir.empty()
        || task->backupDir.empty() || !isSafeRelativePath(task->restartExecutable)) {
        *error = L"invalid update task paths or process id";
        return false;
    }

    const std::regex filePattern(R"json(\{\s*"path"\s*:\s*"([^"]+)"\s*,\s*"size"\s*:\s*([0-9]+)\s*\})json");
    for (std::sregex_iterator it(json.begin(), json.end(), filePattern), end; it != end; ++it) {
        UpdateFile file{fs::path(utf8ToWide(unescapeJsonString((*it)[1].str()))), static_cast<std::uintmax_t>(std::stoull((*it)[2].str()))};
        if (!isSafeRelativePath(file.relativePath)) {
            *error = L"unsafe payload path";
            return false;
        }
        task->files.push_back(std::move(file));
    }
    if (task->files.empty()) {
        *error = L"update task contains no files";
        return false;
    }
    return true;
}

// =====wjy====
bool ensureProcessExited(DWORD processId)
{
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, processId); // wjy: 启动时立即持有任务 PID 的同步和终止权限，避免等待期间 PID 被复用后误操作其它进程。
    if (!process) {
        const DWORD openError = GetLastError(); // wjy: PID 已不存在表示主程序已经自行退出，其它错误必须停止更新以免在未知占用状态下替换文件。
        if (openError == ERROR_INVALID_PARAMETER) {
            logLine(L"main process already exited");
            return true;
        }
        logLine(L"OpenProcess failed win32=" + std::to_wstring(openError));
        return false;
    }

    constexpr DWORD kGracefulExitTimeoutMs = 60000;
    logLine(L"main process wait begin pid=" + std::to_wstring(processId)
        + L" timeout_ms=" + std::to_wstring(kGracefulExitTimeoutMs)); // wjy: 在阻塞等待前立即落盘，和超时日志的时间差就是实际等待时长。
    DWORD waitResult = WaitForSingleObject(process, kGracefulExitTimeoutMs); // wjy: 活跃远控更新会先关闭 WebRTC、采集编码、Bridge、SSH 和 Qt 后台线程；60 秒宽限禁止旧版 15 秒过早强杀留下显卡/驱动状态。
    if (waitResult == WAIT_OBJECT_0) {
        CloseHandle(process);
        logLine(L"main process exited gracefully");
        return true;
    }
    if (waitResult != WAIT_TIMEOUT) {
        logLine(L"main process wait failed win32=" + std::to_wstring(GetLastError()));
        CloseHandle(process);
        return false;
    }

    logLine(L"main process graceful exit timeout; terminating pid=" + std::to_wstring(processId)); // wjy: 窗口消失但清理线程卡住时，由独立更新器结束指定旧进程，避免更新永久等待。
    if (!TerminateProcess(process, ERROR_PROCESS_ABORTED)) {
        logLine(L"TerminateProcess failed win32=" + std::to_wstring(GetLastError()));
        CloseHandle(process);
        return false;
    }

    waitResult = WaitForSingleObject(process, 5000); // wjy: TerminateProcess 是异步请求，确认进程对象已进入终止态后才允许覆盖 EXE 和 DLL。
    CloseHandle(process);
    if (waitResult != WAIT_OBJECT_0) {
        logLine(L"terminated main process did not signal exit");
        return false;
    }
    logLine(L"main process terminated after graceful timeout");
    return true;
}

std::wstring normalizedAbsolutePath(const fs::path& path)
{
    const std::wstring input = path.wstring();
    const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) return {}; // wjy: 无法标准化的路径绝不能参与进程终止判断，避免退化为按文件名误杀。

    std::wstring result(required, L'\0');
    const DWORD written = GetFullPathNameW(input.c_str(), required, result.data(), nullptr);
    if (written == 0 || written >= required) return {};
    result.resize(written);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result; // wjy: 主任务目录和系统返回的进程映像路径都先转成绝对规范形式，再做不区分大小写的完整路径比较。
}

bool pathsEqualInsensitive(const std::wstring& left, const std::wstring& right)
{
    if (left.empty() || right.empty()) return false;
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()), TRUE)
        == CSTR_EQUAL; // wjy: Windows 路径大小写不敏感，但必须逐字符覆盖完整路径，不能只比较 FakerInputBridge.exe 文件名。
}

bool taskUpdatesBridge(const UpdateTask& task)
{
    const fs::path bridgeRelativePath = L"FakerInputBridge.exe";
    for (const UpdateFile& file : task.files) {
        if (file.relativePath.lexically_normal() == bridgeRelativePath) return true;
    }
    return false; // wjy: 回撤或未来精简任务不包含 Bridge 时不干预任何辅助进程。
}

bool stopBridgeAtTargetPath(const UpdateTask& task, std::wstring* error)
{
    if (!taskUpdatesBridge(task)) return true;

    const fs::path targetBridgePath = task.targetDir / L"FakerInputBridge.exe";
    const std::wstring normalizedTarget = normalizedAbsolutePath(targetBridgePath);
    if (normalizedTarget.empty()) {
        *error = L"cannot normalize target FakerInputBridge path";
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        *error = L"cannot enumerate FakerInputBridge processes win32=" + std::to_wstring(GetLastError());
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL hasProcess = Process32FirstW(snapshot, &entry);
    while (hasProcess) {
        if (entry.th32ProcessID != 0 && entry.th32ProcessID != GetCurrentProcessId()) {
            HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                FALSE,
                entry.th32ProcessID);
            if (process) {
                std::wstring imagePath(32768, L'\0');
                DWORD imageLength = static_cast<DWORD>(imagePath.size());
                if (QueryFullProcessImageNameW(process, 0, imagePath.data(), &imageLength)) {
                    imagePath.resize(imageLength);
                    const std::wstring normalizedImage = normalizedAbsolutePath(imagePath);
                    if (pathsEqualInsensitive(normalizedImage, normalizedTarget)) {
                        DWORD waitResult = WaitForSingleObject(process, 2000); // wjy: FSRemote 自己创建的 Bridge 通常会随主进程 Job 自动退出，先等待避免不必要的强制终止。
                        if (waitResult == WAIT_TIMEOUT) {
                            logLine(L"target FakerInputBridge still running; terminating pid="
                                + std::to_wstring(entry.th32ProcessID));
                            if (!TerminateProcess(process, ERROR_PROCESS_ABORTED)) {
                                const DWORD terminateError = GetLastError();
                                CloseHandle(process);
                                CloseHandle(snapshot);
                                *error = L"cannot terminate target FakerInputBridge pid="
                                    + std::to_wstring(entry.th32ProcessID)
                                    + L" win32=" + std::to_wstring(terminateError);
                                return false; // wjy: 权限不足时在复制前停止事务，不能让安装与回滚都撞上同一个文件锁。
                            }
                            waitResult = WaitForSingleObject(process, 5000);
                        }
                        if (waitResult != WAIT_OBJECT_0) {
                            CloseHandle(process);
                            CloseHandle(snapshot);
                            *error = L"target FakerInputBridge did not exit pid="
                                + std::to_wstring(entry.th32ProcessID);
                            return false;
                        }
                        logLine(L"target FakerInputBridge exited pid=" + std::to_wstring(entry.th32ProcessID));
                    }
                }
                CloseHandle(process);
            }
        }
        hasProcess = Process32NextW(snapshot, &entry);
    }
    const DWORD enumerationError = GetLastError();
    CloseHandle(snapshot);
    if (enumerationError != ERROR_NO_MORE_FILES) {
        *error = L"FakerInputBridge process enumeration failed win32=" + std::to_wstring(enumerationError);
        return false;
    }
    return true; // wjy: 只要目标安装目录的 Bridge 已全部退出，更新器即可安全备份、覆盖并在新版 FSRemote 启动后重新创建服务进程。
}
// ===end====

bool copyVerified(const fs::path& source, const fs::path& destination, std::uintmax_t expectedSize)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return false;
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    return !ec && fs::exists(destination, ec) && fs::file_size(destination, ec) == expectedSize;
}

bool rollback(const UpdateTask& task, const std::vector<UpdateFile>& installed)
{
    bool ok = true;
    for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
        const fs::path backup = task.backupDir / it->relativePath;
        const fs::path target = task.targetDir / it->relativePath;
        std::error_code ec;
        if (fs::exists(backup, ec)) {
            ok = copyVerified(backup, target, fs::file_size(backup, ec)) && ok; // wjy: 已有文件从独立备份恢复，保证失败后回到旧版本。
        } else {
            fs::remove(target, ec); // wjy: 更新中新引入的文件没有备份，回滚时删除以免残留混合版本。
            ok = !ec && ok;
        }
    }
    return ok;
}

bool install(const UpdateTask& task, std::wstring* error)
{
    std::error_code ec;
    fs::remove_all(task.backupDir, ec);
    fs::create_directories(task.backupDir, ec);
    if (ec) { *error = L"cannot create backup directory"; return false; }

    for (const UpdateFile& file : task.files) {
        const fs::path source = task.sourceDir / file.relativePath;
        if (!fs::is_regular_file(source, ec) || fs::file_size(source, ec) != file.size) {
            *error = L"staged payload validation failed: " + file.relativePath.wstring();
            return false;
        }
        const fs::path target = task.targetDir / file.relativePath;
        if (fs::exists(target, ec) && !copyVerified(target, task.backupDir / file.relativePath, fs::file_size(target, ec))) {
            *error = L"backup failed: " + file.relativePath.wstring();
            return false;
        }
    }

    std::vector<UpdateFile> installed;
    for (const UpdateFile& file : task.files) {
        installed.push_back(file); // wjy: 先记录本次涉及文件，复制失败时连当前半完成文件也纳入回滚。
        if (!copyVerified(task.sourceDir / file.relativePath, task.targetDir / file.relativePath, file.size)) {
            *error = L"install failed: " + file.relativePath.wstring();
            logLine(*error);
            if (!rollback(task, installed)) logLine(L"rollback incomplete");
            return false;
        }
    }
    return true;
}

bool restartFsRemote(const UpdateTask& task, bool updated)
{
    const fs::path executable = task.targetDir / task.restartExecutable;
    std::wstring command = L"\"" + executable.wstring() + L"\" ";
    command += L"--restart-after-pid " + std::to_wstring(GetCurrentProcessId()) + L" "; // wjy: 新主程序先等待更新器关闭 data/updater.log，再执行本轮统一日志清理。
    command += updated
        ? L"--updated-from \"" + task.fromVersion + L"\" --updated-to \"" + task.toVersion + L"\" --minimized" // wjy: 更新成功后的新主程序直接进入托盘，避免更新重启打断用户当前桌面。
        : L"--update-rollback \"" + task.toVersion + L"\""; // wjy: 成功参数仅记录已安装版本并静默启动，回滚参数继续触发主程序失败警告。
    STARTUPINFOW startup{sizeof(startup)};
    // =====wjy====
    startup.dwFlags = STARTF_USESHOWWINDOW; // wjy: 明确要求 Windows 使用下面的启动显示状态，避免新进程在 Qt 解析参数前先按普通窗口创建。
    startup.wShowWindow = updated ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL; // wjy: 成功更新从创建阶段保持最小化且不抢焦点，回滚仍按普通窗口启动以便显示失败提示。
    // ===end====
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        task.targetDir.c_str(), &startup, &process);
    if (created) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    if (!created) {
        logLine(L"CreateProcess failed win32=" + std::to_wstring(GetLastError())); // wjy: 自动重启失败时记录系统错误码，区分路径、权限和依赖加载问题。
    }
    return created == TRUE;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3 || std::wstring(argv[1]) != L"--task") return 2;
    const fs::path taskPath = fs::absolute(argv[2]);
    UpdateTask task;
    std::wstring error;
    if (!parseTask(taskPath, &task, &error)) { logLine(error); return 3; }
    // =====wjy====
    std::error_code logDirectoryError;
    const fs::path logDirectory = task.targetDir / L"data"; // wjy: 更新器日志归属目标安装，不再散落在 AppData 的版本任务目录。
    fs::create_directories(logDirectory, logDirectoryError);
    if (!logDirectoryError) {
        g_log.open(logDirectory / L"updater.log", std::ios::out | std::ios::trunc); // wjy: 每次更新器运行从空文件开始；新主程序启动后还会按统一规则清除上一进程日志。
    }
    // ===end====
    logLine(L"task parsed pid=" + std::to_wstring(task.processId)); // wjy: 日志明确区分任务解析、等待、安装和重启阶段。
    if (!ensureProcessExited(task.processId)) { logLine(L"main process could not be stopped safely"); return 4; } // wjy: 无论优雅退出还是超时强制结束，都必须确认旧进程已停止后才能进入安装。
    logLine(L"main process exited");
    logLine(L"target bridge cleanup begin"); // wjy: 将 Bridge 清理与主进程等待分段计时，确认外部输入桥是否造成额外延迟。
    if (!stopBridgeAtTargetPath(task, &error)) { logLine(error); return 4; } // wjy: 主进程退出后只清理本安装目录仍存活的外部 Bridge，解除本次真实的 FakerInputBridge.exe 覆盖失败。
    logLine(L"target bridge cleanup end");
    logLine(L"install begin files=" + std::to_wstring(task.files.size())); // wjy: 记录安装文件数量并为备份、覆盖和回滚阶段建立起点。
    const bool updated = install(task, &error);
    logLine(updated ? L"update installed" : error);
    logLine(L"restart begin");
    if (!restartFsRemote(task, updated)) { logLine(L"restart failed"); return 6; }
    logLine(L"restart started");
    return updated ? 0 : 5;
}
// ===end====
