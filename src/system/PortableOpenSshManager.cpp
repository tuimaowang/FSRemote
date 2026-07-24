#include "system/PortableOpenSshManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <utility> // wjy: 转移后台取消回调的所有权，避免长脚本执行期间额外复制状态闭包。

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace platform {
namespace {

constexpr uint16_t kPortableSshPort = 49103;

// =====wjy====
QString currentProcessAccount()
{
#if !defined(_WIN32)
    return qEnvironmentVariable("USER").trimmed();
#else
    wchar_t account[256] = {};
    DWORD accountSize = static_cast<DWORD>(std::size(account));
    if (!GetUserNameW(account, &accountSize) || accountSize <= 1) return {};
    return QString::fromWCharArray(account, static_cast<qsizetype>(accountSize - 1)).trimmed(); // wjy: ACL 必须授予实际进程令牌账户，不能假设 USERNAME 环境变量与运行身份相同。
#endif
}
// ===end====

QString quoteForCmd(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString quoteForSshdConfigPath(const QString& path)
{
    QString normalized = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    normalized.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(normalized); // wjy: sshd_config 按空格切分参数，目录名带空格或括号时必须给路径加引号。
}

QStringList sshArguments(const QString& keyPath, uint16_t port, const QString& loginUser, const QString& hostIp)
{
    return {
        QStringLiteral("-tt"),
        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=no"),
        QStringLiteral("-o"), QStringLiteral("UserKnownHostsFile=NUL"),
        QStringLiteral("-o"), QStringLiteral("PreferredAuthentications=publickey"),
        QStringLiteral("-o"), QStringLiteral("PubkeyAuthentication=yes"),
        QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=5"),
        QStringLiteral("-o"), QStringLiteral("ConnectionAttempts=3"),
        QStringLiteral("-i"), keyPath,
        QStringLiteral("-p"), QString::number(port),
        QStringLiteral("-l"), loginUser,
        hostIp,
    };
}

// =====wjy====
QString powerShellEncodedCommand(const QString& script)
{
    const QByteArray utf16LittleEndian(
        reinterpret_cast<const char*>(script.utf16()),
        script.size() * int(sizeof(ushort)));
    return QString::fromLatin1(utf16LittleEndian.toBase64()); // wjy: 这里只编码很短的“Base64 文件转 ps1”引导脚本，不再把完整业务脚本塞进命令行。
}
// ===end====

QString readLogTail(const QString& path, int maxBytes = 4096)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    if (file.size() > maxBytes) {
        file.seek(file.size() - maxBytes);
    }
    return QString::fromLocal8Bit(file.readAll()).trimmed();
}

// =====wjy====
QByteArray normalizedPublicKeyLine(const QByteArray& key)
{
    return key.trimmed(); // wjy: authorized_keys 按单行公钥保存，这里统一去掉首尾空白，避免比较和追加时出现重复行。
}

bool authorizedKeysContain(const QByteArray& existing, const QByteArray& publicKey)
{
    const QByteArray normalizedKey = normalizedPublicKeyLine(publicKey); // wjy: 用规范化后的公钥行和文件中每一行比较。
    const QList<QByteArray> lines = existing.split('\n');
    for (const QByteArray& line : lines) {
        if (normalizedPublicKeyLine(line) == normalizedKey) {
            return true; // wjy: 目标 authorized_keys 已经包含这把公钥时不再重复追加。
        }
    }
    return false;
}

QByteArray appendAuthorizedKeyLine(QByteArray existing, const QByteArray& publicKey)
{
    const QByteArray normalizedKey = normalizedPublicKeyLine(publicKey); // wjy: 追加前再次规范化，保证写入的每个 key 都是一行。
    existing = existing.trimmed();
    if (!existing.isEmpty()) {
        existing.append('\n'); // wjy: 保留原有远程授权 key，并在末尾换行后追加新的设备公钥。
    }
    existing.append(normalizedKey);
    existing.append('\n');
    return existing;
}
// ===end====

} // namespace

PortableOpenSshManager& PortableOpenSshManager::instance()
{
    static PortableOpenSshManager manager;
    return manager;
}

PortableOpenSshManager::~PortableOpenSshManager()
{
    stopClientProcesses(); // wjy: 静态单例析构再兜底一次，异常退出顺序下也不遗留交互式 ssh.exe。
    stopServer();
}

// =====wjy====
bool PortableOpenSshManager::ensureServerProcessJob(QString* errorMessage)
{
#if !defined(_WIN32)
    Q_UNUSED(errorMessage);
    return true;
#else
    if (m_serverProcessJob) {
        return true;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建 SSH 服务进程组，错误码 %1").arg(GetLastError());
        }
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // wjy: 正常关闭句柄或 FSRemote 被 TerminateProcess 强制结束时，Windows 都会自动结束 sshd。
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const DWORD error = GetLastError();
        CloseHandle(job);
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法设置 SSH 服务退出清理规则，错误码 %1").arg(error);
        }
        return false;
    }

    m_serverProcessJob = job;
    return true;
#endif
}

bool PortableOpenSshManager::attachServerProcessToJob(qint64 processId, QString* errorMessage)
{
#if !defined(_WIN32)
    Q_UNUSED(processId);
    Q_UNUSED(errorMessage);
    return true;
#else
    HANDLE job = reinterpret_cast<HANDLE>(m_serverProcessJob);
    if (!job || processId <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("SSH 服务进程组或进程 ID 无效。");
        }
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(processId));
    if (!process) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法打开 SSH 服务进程，错误码 %1").arg(GetLastError());
        }
        return false;
    }

    const BOOL attached = AssignProcessToJobObject(job, process); // wjy: 绑定完成后 sshd 及其未来子进程都受同一退出生命周期约束。
    const DWORD attachError = attached ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!attached) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法管理 SSH 服务进程，错误码 %1").arg(attachError);
        }
        return false;
    }
    return true;
#endif
}

void PortableOpenSshManager::closeServerProcessJob()
{
#if defined(_WIN32)
    HANDLE job = reinterpret_cast<HANDLE>(m_serverProcessJob);
    if (!job) {
        return;
    }
    m_serverProcessJob = nullptr; // wjy: 先清空成员，使托盘退出、main 兜底和单例析构连续调用时不会重复操作旧句柄。
    TerminateJobObject(job, 0); // wjy: 主动退出立即结束 Job 中的 sshd/子进程，确保 openssh 目录不再被占用。
    CloseHandle(job); // wjy: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 同时覆盖主进程异常死亡、来不及执行显式清理的路径。
#endif
}

bool PortableOpenSshManager::ensureClientProcessJob(QString* errorMessage)
{
#if !defined(_WIN32)
    Q_UNUSED(errorMessage);
    return true;
#else
    if (m_clientProcessJob) {
        return true;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建终端进程组，错误码 %1").arg(GetLastError());
        }
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // wjy: FSRemote 关闭 Job 句柄时由系统终止其中 cmd.exe 以及继承 Job 的 ssh.exe。
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const DWORD error = GetLastError();
        CloseHandle(job);
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法设置终端退出清理规则，错误码 %1").arg(error);
        }
        return false;
    }

    m_clientProcessJob = job;
    return true;
#endif
}

void PortableOpenSshManager::stopClientProcesses()
{
    std::lock_guard identityLock(m_identityMutex); // wjy: 后台终端可能正在创建或绑定客户端 Job，退出清理必须与 openTerminal 串行操作同一句柄。
#if defined(_WIN32)
    HANDLE job = reinterpret_cast<HANDLE>(m_clientProcessJob);
    if (!job) {
        return;
    }
    m_clientProcessJob = nullptr; // wjy: 先清空成员保证托盘退出、main 和析构重复调用时保持幂等。
    TerminateJobObject(job, 0); // wjy: 主动结束 Job 内全部交互终端，随后关闭句柄完成系统资源释放。
    CloseHandle(job);
#endif
}
// ===end====

bool PortableOpenSshManager::startServer(QString* errorMessage)
{
    if (!ensurePrepared(errorMessage)) {
        return false;
    }

    if (m_sshdProcess && m_sshdProcess->state() != QProcess::NotRunning) {
        return true;
    }

    stopServer();
    if (!cleanupResidualServerProcesses(errorMessage)) {
        return false;
    }
    if (!ensureServerProcessJob(errorMessage)) {
        return false; // wjy: 无法建立退出兜底时不启动 sshd，禁止再次产生锁住版本目录的游离服务进程。
    }

    auto* process = new QProcess();
    process->setProgram(sshdExePath());
    process->setArguments({QStringLiteral("-D"), QStringLiteral("-f"), sshdConfigPath(), QStringLiteral("-E"), sshdLogPath()});
    process->setWorkingDirectory(opensshDir());

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString pathKey = environment.contains(QStringLiteral("Path")) ? QStringLiteral("Path") : QStringLiteral("PATH");
    const QString pathValue = environment.value(pathKey);
    environment.insert(pathKey, opensshDir() + QLatin1Char(';') + pathValue);
    process->setProcessEnvironment(environment);
    process->start();
    if (!process->waitForStarted(2500)) {
        if (errorMessage) {
            *errorMessage = process->errorString();
            const QString logTail = readLogTail(sshdLogPath());
            if (!logTail.isEmpty()) {
                *errorMessage += QStringLiteral("\n") + logTail;
            }
        }
        delete process;
        closeServerProcessJob(); // wjy: 启动失败时关闭尚未装入进程的空 Job，避免句柄泄漏。
        return false;
    }

    if (!attachServerProcessToJob(process->processId(), errorMessage)) {
        process->kill(); // wjy: Job 绑定失败时立即结束刚启动的 sshd，不能让它脱离 FSRemote 生命周期继续占用目录。
        process->waitForFinished(1200);
        delete process;
        closeServerProcessJob();
        return false;
    }

    process->waitForReadyRead(200);
    if (process->state() == QProcess::NotRunning) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("sshd exited unexpectedly");
            const QString logTail = readLogTail(sshdLogPath());
            if (!logTail.isEmpty()) {
                *errorMessage += QStringLiteral("\n") + logTail;
            }
        }
        delete process;
        closeServerProcessJob(); // wjy: sshd 自行退出后同步释放 Job，下次启动会创建全新的受控进程组。
        return false;
    }

    m_sshdProcess = process;
    return true;
}

void PortableOpenSshManager::stopServer()
{
    if (!m_sshdProcess) {
        closeServerProcessJob(); // wjy: QProcess 指针已经丢失时仍关闭 Job，可清理异常路径留下但已经纳入管理的 sshd。
        cleanupResidualServerProcesses(nullptr); // wjy: 再按当前版本目录的 sshd.exe 绝对路径清理，兼容旧版本遗留或启动绑定竞态。
        return;
    }

    closeServerProcessJob(); // wjy: 退出目标是立即释放版本目录，优先结束整个 sshd Job，不再等待控制台程序处理普通 terminate。
    if (!m_sshdProcess->waitForFinished(1200)) {
        m_sshdProcess->kill(); // wjy: Job API 异常失败时保留 QProcess 最后一层兜底，退出不能无限等待。
        m_sshdProcess->waitForFinished(1200);
    }
    delete m_sshdProcess;
    m_sshdProcess = nullptr;
    cleanupResidualServerProcesses(nullptr); // wjy: 扫描只终止路径完全等于本版本 openssh/sshd.exe 的进程，不影响系统或其它目录中的 OpenSSH 服务。
}

bool PortableOpenSshManager::openTerminal(const QString& hostIp, const QString& loginUser, QString* errorMessage)
{
    std::lock_guard identityLock(m_identityMutex); // wjy: 后台批量终端串行保护 OpenSSH 准备、客户端 Job 创建和进程绑定，避免多线程重复创建或关闭句柄。
    const QString normalizedHost = hostIp.trimmed();
    const QString normalizedUser = loginUser.trimmed();
    if (normalizedHost.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("target host is empty");
        }
        return false;
    }
    if (normalizedUser.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("target login user is empty");
        }
        return false;
    }
    if (!ensurePrepared(errorMessage)) {
        return false;
    }

    const QStringList arguments = sshArguments(
        QDir::toNativeSeparators(effectiveClientKeyPath()),
        serverPort(),
        normalizedUser,
        normalizedHost);

#if defined(_WIN32)
    if (!ensureClientProcessJob(errorMessage)) {
        return false; // wjy: 无法纳入退出管理时不启动游离终端，避免用户退出 FSRemote 后残留 ssh.exe。
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION processInfo{};
// =====wjy====
    std::wstring applicationName = QDir::toNativeSeparators(shellPath()).toStdWString(); // wjy: 通过 cmd.exe 承载 ssh.exe，连接失败时保留窗口显示错误原因，避免黑窗一闪而过。
    QStringList commandParts;
    commandParts.reserve(arguments.size() + 1);
    commandParts.push_back(quoteForCmd(QDir::toNativeSeparators(sshExePath())));
    for (const QString& argument : arguments) {
        commandParts.push_back(argument.contains(QLatin1Char(' ')) ? quoteForCmd(argument) : argument);
    }
    const QString sshCommand = commandParts.join(QLatin1Char(' ')); // wjy: 保持原有 ssh.exe 参数不变，只把它包进 cmd /k。
    const QString cmdCommandLine = quoteForCmd(QDir::toNativeSeparators(shellPath()))
        + QStringLiteral(" /k \"")
        + sshCommand
        + QLatin1Char('"'); // wjy: /k 会在 ssh.exe 正常退出或失败后继续停留，方便现场读取认证/网络错误。
    std::wstring mutableCommandLine = cmdCommandLine.toStdWString();
    std::wstring workingDirectory = QDir::toNativeSeparators(appDir()).toStdWString();
// ===end====

    const BOOL created = CreateProcessW(
        applicationName.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE | CREATE_SUSPENDED, // wjy: 先暂停 cmd.exe，加入 Job 后再运行，避免它提前创建未受管理的 ssh.exe 子进程。
        nullptr,
        workingDirectory.c_str(),
        &startupInfo,
        &processInfo);
    if (!created) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to start terminal process (%1)").arg(GetLastError());
        }
        return false;
    }

    if (!AssignProcessToJobObject(reinterpret_cast<HANDLE>(m_clientProcessJob), processInfo.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(processInfo.hProcess, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法管理终端进程，错误码 %1").arg(error);
        }
        return false; // wjy: 绑定失败时直接终止仍处于暂停状态的 cmd，绝不留下无法随程序退出的终端。
    }
    if (ResumeThread(processInfo.hThread) == DWORD(-1)) {
        const DWORD error = GetLastError();
        TerminateProcess(processInfo.hProcess, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法启动受管理的终端进程，错误码 %1").arg(error);
        }
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
#else
    if (!QProcess::startDetached(
            sshExePath(),
            arguments,
            appDir())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to start terminal process");
        }
        return false;
    }
    return true;
#endif
}

bool PortableOpenSshManager::runRemoteCommands(const QString& hostIp, const QString& loginUser, const QStringList& commands, QString* outputText, QString* errorMessage, int timeoutMs, std::function<void(const QString&)> outputCallback, std::function<bool()> shouldCancel)
{
// =====wjy====
    const QString normalizedHost = hostIp.trimmed();
    const QString normalizedUser = loginUser.trimmed();
    if (normalizedHost.isEmpty() || normalizedUser.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("target host or login user is empty");
        }
        return false; // wjy: 远程命令必须有目标 IP 和登录用户，否则 ssh.exe 无法建立会话。
    }
    if (commands.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("remote command is empty");
        }
        return false; // wjy: 没有命令时不启动 SSH，避免打开一个空的远程 cmd 会话。
    }
    if (!ensurePrepared(errorMessage)) {
        return false;
    }

    QStringList arguments = sshArguments(
        QDir::toNativeSeparators(effectiveClientKeyPath()),
        serverPort(),
        normalizedUser,
        normalizedHost);

    QProcess process;
    process.setProgram(sshExePath());
    process.setArguments(arguments);
    process.setWorkingDirectory(appDir());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString pathKey = environment.contains(QStringLiteral("Path")) ? QStringLiteral("Path") : QStringLiteral("PATH");
    const QString pathValue = environment.value(pathKey);
    environment.insert(pathKey, opensshDir() + QLatin1Char(';') + pathValue);
    process.setProcessEnvironment(environment);
    process.start();
    if (!process.waitForStarted(5000)) {
        if (errorMessage) {
            *errorMessage = process.errorString();
        }
        return false;
    }

    QString commandText;
    for (const QString& command : commands) {
        commandText += command;
        commandText += QStringLiteral("\r\n"); // wjy: 远端 ForceCommand 是 cmd.exe，把每条命令像终端输入一样写入 stdin。
    }
    commandText += QStringLiteral("exit\r\n");
    process.write(commandText.toLocal8Bit());
    process.closeWriteChannel();

    QByteArray collectedOutput;
    auto collectProcessOutput = [&process, &collectedOutput, &outputCallback] {
        const QByteArray chunk = process.readAllStandardOutput()
            + process.readAllStandardError();
        if (chunk.isEmpty()) {
            return;
        }
        collectedOutput += chunk;
        if (outputCallback) {
            outputCallback(QString::fromUtf8(chunk)); // wjy: Script output is streamed back as UTF-8; remote PowerShell script sets Console.OutputEncoding accordingly.
        }
    };
    QElapsedTimer commandTimer;
    commandTimer.start();
    bool canceled = false;
    while (process.state() != QProcess::NotRunning) {
        if (shouldCancel && shouldCancel()) {
            canceled = true;
            break;
        }
        if (process.waitForFinished(200)) {
            break;
        }
        collectProcessOutput();
        if (timeoutMs > 0 && commandTimer.elapsed() > timeoutMs) {
            break;
        }
    }
    collectProcessOutput();

    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(3000);
        collectProcessOutput();
        if (outputText) {
            *outputText = QString::fromUtf8(collectedOutput).trimmed();
        }
        if (errorMessage) {
            *errorMessage = canceled
                ? QStringLiteral("remote command canceled")
                : QStringLiteral("remote command timed out");
        }
        return false;
    }

    const QString output = QString::fromUtf8(collectedOutput);
    if (outputText) {
        *outputText = output.trimmed();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = output.trimmed().isEmpty()
                ? QStringLiteral("remote command failed with exit code %1").arg(process.exitCode())
                : output.trimmed();
        }
        return false;
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
// ===end====
}

bool PortableOpenSshManager::runRemotePowerShellScript(
    const QString& hostIp,
    const QString& loginUser,
    const QString& script,
    QString* outputText,
    QString* errorMessage,
    int timeoutMs,
    std::function<void(const QString&)> outputCallback,
    std::function<bool()> shouldCancel)
{
// =====wjy====
    if (script.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("remote PowerShell script is empty");
        }
        return false; // wjy: 空脚本不创建远端临时文件，避免返回一次看似成功但没有实际执行的任务。
    }

    QString transferId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    transferId.remove(QLatin1Char('-')); // wjy: 临时文件名只保留十六进制字符，放入 cmd 和 PowerShell 字符串时无需额外转义。
    const QString base64FileName = QStringLiteral("fsremote_%1.ps1.b64").arg(transferId);
    const QString scriptFileName = QStringLiteral("fsremote_%1.ps1").arg(transferId);
    const QString outputBeginMarker = QStringLiteral("FSREMOTE_SCRIPT_OUTPUT_BEGIN_%1").arg(transferId);
    const QString outputEndMarker = QStringLiteral("FSREMOTE_SCRIPT_OUTPUT_END_%1").arg(transferId);

    QByteArray scriptBytes("\xEF\xBB\xBF", 3);
    scriptBytes.append(script.toUtf8()); // wjy: 写入 UTF-8 BOM，让目标 Windows PowerShell 5.1 也能稳定识别中文路径和中文注释。
    const QByteArray scriptBase64 = scriptBytes.toBase64();

    constexpr qsizetype kBase64ChunkSize = 2048;
    const qsizetype chunkCount = (scriptBase64.size() + kBase64ChunkSize - 1) / kBase64ChunkSize;
    QStringList commands;
    commands.reserve(chunkCount + 8);
    for (qsizetype offset = 0; offset < scriptBase64.size(); offset += kBase64ChunkSize) {
        const QString chunk = QString::fromLatin1(scriptBase64.mid(offset, kBase64ChunkSize));
        const QString redirection = offset == 0 ? QStringLiteral(">") : QStringLiteral(">>");
        commands.append(QStringLiteral("@%1 \"%TEMP%\\%2\" echo %3")
            .arg(redirection, base64FileName, chunk)); // wjy: 每行最多约 2KB，远低于 cmd 8191 字符上限；Base64 不含 &、|、% 等 cmd 元字符。
    }

    const QString decodeScript = QStringLiteral(
        "$base64Path = Join-Path $env:TEMP '%1'\n"
        "$scriptPath = Join-Path $env:TEMP '%2'\n"
        "$bytes = [Convert]::FromBase64String([IO.File]::ReadAllText($base64Path))\n"
        "[IO.File]::WriteAllBytes($scriptPath, $bytes)")
        .arg(base64FileName, scriptFileName); // wjy: 短引导脚本只负责把分块 Base64 还原成带 BOM 的 ps1 文件。
    commands.append(QStringLiteral("@powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand %1")
        .arg(powerShellEncodedCommand(decodeScript))); // wjy: 引导命令长度固定且很短，不会再次触发 EncodedCommand 截断。
    commands.append(QStringLiteral("@if errorlevel 1 exit 9008")); // wjy: 临时脚本还原失败时立即把非零退出码传回控制端，不继续执行残缺文件。
    commands.append(QStringLiteral("@echo %1").arg(outputBeginMarker)); // wjy: 从这个唯一标记之后才把远端输出交给脚本面板，上传 Base64 和 cmd 回显全部丢弃。
    commands.append(QStringLiteral("@powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%TEMP%\\%1\"")
        .arg(scriptFileName)); // wjy: 真正业务脚本从文件执行，脚本再长也不占用 cmd 单条命令长度。
    commands.append(QStringLiteral("@set \"FSREMOTE_POWERSHELL_EXIT=%ERRORLEVEL%\"")); // wjy: 删除临时文件前保存业务脚本退出码，保证失败不会被 del 命令覆盖。
    commands.append(QStringLiteral("@del /q \"%TEMP%\\%1\" \"%TEMP%\\%2\" >NUL 2>&1")
        .arg(base64FileName, scriptFileName)); // wjy: 正常结束后删除本次唯一命名的 Base64 和 ps1 临时文件。
    commands.append(QStringLiteral("@echo %1").arg(outputEndMarker)); // wjy: 结束标记之后的提示符和 exit 回显不进入脚本日志。
    commands.append(QStringLiteral("@exit %FSREMOTE_POWERSHELL_EXIT%")); // wjy: 让 ssh.exe 返回业务脚本真实退出码，UI 不再把 PowerShell 解析失败误显示成已完成。

    QString rawOutput;
    QString rawError;
    QString pendingLine;
    QString filteredOutput;
    bool outputStarted = false;
    bool outputFinished = false;
    bool streamedAnyOutput = false;
    auto filteredCallback = [&](const QString& chunk) {
        pendingLine += chunk;
        qsizetype newlineIndex = -1;
        while ((newlineIndex = pendingLine.indexOf(QLatin1Char('\n'))) >= 0) {
            const QString line = pendingLine.left(newlineIndex + 1);
            pendingLine.remove(0, newlineIndex + 1);
            const bool isBeginMarker = line.contains(outputBeginMarker)
                && !line.contains(QStringLiteral("echo %1").arg(outputBeginMarker), Qt::CaseInsensitive);
            const bool isEndMarker = line.contains(outputEndMarker)
                && !line.contains(QStringLiteral("echo %1").arg(outputEndMarker), Qt::CaseInsensitive);
            if (!outputStarted) {
                if (isBeginMarker) {
                    outputStarted = true; // wjy: 忽略 SSH 欢迎语、远端 prompt、分块上传命令以及它们的 PTY 回显。
                }
                continue;
            }
            if (isEndMarker) {
                outputFinished = true;
                outputStarted = false;
                continue;
            }
            if (!outputFinished) {
                filteredOutput += line;
                if (outputCallback) {
                    outputCallback(line);
                    streamedAnyOutput = true; // wjy: 标记内容已经实时送到 UI，结束后不再把同一段结果重复推送一次。
                }
            }
        }
    };

    const bool ok = runRemoteCommands(
        hostIp,
        loginUser,
        commands,
        &rawOutput,
        &rawError,
        timeoutMs,
        filteredCallback,
        std::move(shouldCancel)); // wjy: 保留原有 PTY 生命周期和取消语义，只在输出层隔离传输噪声。
    filteredCallback(QStringLiteral("\n")); // wjy: 命令结束时补一个换行，处理最后一段没有换行符的脚本输出或结束标记。

    if (!outputStarted && filteredOutput.isEmpty()) {
        const qsizetype beginPosition = rawOutput.lastIndexOf(outputBeginMarker);
        const qsizetype endPosition = rawOutput.lastIndexOf(outputEndMarker);
        if (beginPosition >= 0) {
            const qsizetype contentStart = beginPosition + outputBeginMarker.size();
            const qsizetype contentLength = endPosition > contentStart ? endPosition - contentStart : -1;
            filteredOutput = rawOutput.mid(contentStart, contentLength).trimmed(); // wjy: 极端终端控制码破坏分行时，用最后一对标记从完整输出中兜底提取业务内容。
            if (outputCallback && !streamedAnyOutput && !filteredOutput.isEmpty()) {
                outputCallback(filteredOutput);
            }
        }
    }

    if (outputText) {
        *outputText = filteredOutput.trimmed(); // wjy: 调用者只拿到脚本输出，不再看到 Base64、Windows banner、prompt 或完整 powershell 命令。
    }
    if (!ok) {
        if (errorMessage) {
            const QString cleanError = filteredOutput.trimmed();
            *errorMessage = cleanError.isEmpty()
                ? QStringLiteral("remote PowerShell script transfer or execution failed")
                : cleanError; // wjy: 失败信息也使用过滤后的业务输出，禁止把超长 Base64 再作为错误文本塞进终端面板。
        }
        return false;
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
// ===end====
}

QString PortableOpenSshManager::clientPublicKey(QString* errorMessage)
{
// =====wjy====
    std::lock_guard identityLock(m_identityMutex); // wjy: 读取公钥前和其它签名/验签操作串行，避免首次准备目录和密钥时发生竞态。
    if (!ensurePrepared(errorMessage)) {
        return {}; // wjy: 取公钥前先完成 OpenSSH 布局、密钥和权限准备，保证后续发给目标设备的 key 是当前可用的。
    }

    QFile publicKeyFile(effectiveClientPublicKeyPath());
    if (!publicKeyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to read client public key");
        }
        return {}; // wjy: 公钥读不到时不能继续授权目标设备，否则 SSH 仍然会认证失败。
    }

    const QString publicKey = QString::fromUtf8(publicKeyFile.readAll()).trimmed();
    if (publicKey.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("client public key is empty");
    }
    return publicKey; // wjy: 返回单行公钥，调用方通过命令通道写入目标机 authorized_keys。
// ===end====
}

bool PortableOpenSshManager::authorizeClientPublicKey(const QString& publicKey, QString* errorMessage)
{
// =====wjy====
    std::lock_guard identityLock(m_identityMutex); // wjy: 写 authorized_keys 时阻止并发验签读取到半次更新。
    if (!ensurePrepared(errorMessage)) {
        return false; // wjy: 写 authorized_keys 前确保 data/openssh 目录和基础配置已经创建完成。
    }

    const QByteArray normalizedKey = normalizedPublicKeyLine(publicKey.toUtf8());
    if (normalizedKey.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("remote client public key is empty");
        }
        return false; // wjy: 空公钥没有授权意义，直接拒绝写入目标机 authorized_keys。
    }

    QFile authorizedKeysFile(authorizedKeysPath());
    QByteArray existing;
    if (authorizedKeysFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        existing = authorizedKeysFile.readAll();
        authorizedKeysFile.close();
    }
    if (authorizedKeysContain(existing, normalizedKey)) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true; // wjy: 发起方公钥已登记时直接返回成功，避免每次打开终端都重复写文件。
    }

    QSaveFile saveFile(authorizedKeysPath());
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to write authorized_keys");
        }
        return false;
    }
    saveFile.write(appendAuthorizedKeyLine(existing, normalizedKey)); // wjy: 保留目标机已有授权 key，再追加当前发起设备的公钥。
    if (!saveFile.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to commit authorized_keys");
        }
        return false;
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
// ===end====
}

// =====wjy====
QByteArray PortableOpenSshManager::signSessionChallenge(const QByteArray& challenge, QString* errorMessage)
{
    std::lock_guard identityLock(m_identityMutex);
    if (challenge.isEmpty() || challenge.size() > 16 * 1024) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid session challenge size");
        return {}; // wjy: 只签名协议定义的小型上下文，拒绝空数据和异常超长输入。
    }
    if (!ensurePrepared(errorMessage)) return {};

    QTemporaryDir temporaryDir(QDir(dataDir()).filePath(QStringLiteral("session-sign-XXXXXX")));
    if (!temporaryDir.isValid()) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create session signing directory");
        return {};
    }
    const QString challengePath = temporaryDir.filePath(QStringLiteral("challenge.bin"));
    QFile challengeFile(challengePath);
    if (!challengeFile.open(QIODevice::WriteOnly) || challengeFile.write(challenge) != challenge.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to write session challenge");
        return {};
    }
    challengeFile.close();

    if (!runTool(sshKeygenExePath(), {
            QStringLiteral("-Y"), QStringLiteral("sign"),
            QStringLiteral("-f"), effectiveClientKeyPath(),
            QStringLiteral("-n"), QStringLiteral("fsremote-session"),
            challengePath,
        }, 8000, errorMessage)) {
        return {};
    }

    QFile signatureFile(challengePath + QStringLiteral(".sig"));
    if (!signatureFile.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to read session signature");
        return {};
    }
    const QByteArray signature = signatureFile.readAll();
    if (signature.isEmpty() || signature.size() > 16 * 1024) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid session signature size");
        return {};
    }
    if (errorMessage) errorMessage->clear();
    return signature; // wjy: 返回 OpenSSH armored SSH signature，协议层会百分号转义后发送。
}

bool PortableOpenSshManager::isSessionPublicKeyAuthorized(const QString& publicKey, QString* errorMessage)
{
    std::lock_guard identityLock(m_identityMutex);
    if (!ensurePrepared(errorMessage)) return false;
    const QByteArray normalizedKey = normalizedPublicKeyLine(publicKey.toUtf8());
    if (normalizedKey.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("session public key is empty");
        return false;
    }
    QFile file(authorizedKeysPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to read authorized_keys");
        return false;
    }
    const bool authorized = authorizedKeysContain(file.readAll(), normalizedKey); // wjy: 必须命中受控设备已经登记的完整公钥行，不能只信任客户端自报指纹。
    if (errorMessage) {
        *errorMessage = authorized ? QString() : QStringLiteral("session public key is not authorized");
    }
    return authorized;
}

bool PortableOpenSshManager::verifySessionChallenge(
    const QString& publicKey,
    const QByteArray& challenge,
    const QByteArray& signature,
    QString* errorMessage)
{
    std::lock_guard identityLock(m_identityMutex);
    if (challenge.isEmpty() || challenge.size() > 16 * 1024 || signature.isEmpty() || signature.size() > 16 * 1024) {
        if (errorMessage) *errorMessage = QStringLiteral("invalid session proof size");
        return false;
    }
    if (!isSessionPublicKeyAuthorized(publicKey, errorMessage)) return false;

    QTemporaryDir temporaryDir(QDir(dataDir()).filePath(QStringLiteral("session-verify-XXXXXX")));
    if (!temporaryDir.isValid()) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to create session verification directory");
        return false;
    }
    const QString challengePath = temporaryDir.filePath(QStringLiteral("challenge.bin"));
    const QString signaturePath = temporaryDir.filePath(QStringLiteral("challenge.sig"));
    const QString allowedSignersPath = temporaryDir.filePath(QStringLiteral("allowed_signers"));
    QFile challengeFile(challengePath);
    QFile signatureFile(signaturePath);
    QFile allowedSignersFile(allowedSignersPath);
    if (!challengeFile.open(QIODevice::WriteOnly)
        || challengeFile.write(challenge) != challenge.size()
        || !signatureFile.open(QIODevice::WriteOnly)
        || signatureFile.write(signature) != signature.size()
        || !allowedSignersFile.open(QIODevice::WriteOnly | QIODevice::Text)
        || allowedSignersFile.write("fsremote-session " + normalizedPublicKeyLine(publicKey.toUtf8()) + "\n") <= 0) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to prepare session verification files");
        return false;
    }
    challengeFile.close();
    signatureFile.close();
    allowedSignersFile.close();
    return runToolWithStandardInput(sshKeygenExePath(), {
        QStringLiteral("-Y"), QStringLiteral("verify"),
        QStringLiteral("-f"), allowedSignersPath,
        QStringLiteral("-I"), QStringLiteral("fsremote-session"),
        QStringLiteral("-n"), QStringLiteral("fsremote-session"),
        QStringLiteral("-s"), signaturePath,
    }, challengePath, 8000, errorMessage); // wjy: ssh-keygen 从标准输入读取原挑战，并验证命名空间、身份、公钥和签名四项绑定。
}
// ===end====

QString PortableOpenSshManager::loginUser() const
{
    return currentLoginUser();
}

uint16_t PortableOpenSshManager::serverPort() const
{
    return kPortableSshPort;
}

bool PortableOpenSshManager::ensurePrepared(QString* errorMessage)
{
    if (m_prepared) {
        return true;
    }
    if (!ensureLayout(errorMessage)) {
        return false;
    }
    if (!QDir().mkpath(dataDir())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to create openssh data directory");
        }
        return false;
    }
    if (!ensureKeys(errorMessage)) {
        return false;
    }
    if (!ensureConfig(errorMessage)) {
        return false;
    }
    m_prepared = true;
    return true;
}

bool PortableOpenSshManager::ensureLayout(QString* errorMessage) const
{
    const QStringList requiredFiles = {
        sshExePath(),
        sshdExePath(),
        sshKeygenExePath(),
        sftpServerExePath(),
    };
    for (const QString& path : requiredFiles) {
        if (!QFileInfo::exists(path)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("missing OpenSSH runtime: %1").arg(QDir::toNativeSeparators(path));
            }
            return false;
        }
    }
    return true;
}

bool PortableOpenSshManager::ensureKeys(QString* errorMessage)
{
    if (!QFileInfo::exists(hostKeyPath())) {
        if (!runTool(sshKeygenExePath(), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(), QStringLiteral("-f"), hostKeyPath()}, 15000, errorMessage)) {
            return false;
        }
    }
    if (!ensurePrivateKeyPermissions(hostKeyPath(), errorMessage)) {
        return false;
    }

    const bool hasBundledSharedKey =
        QFileInfo::exists(bundledClientKeyPath())
        && QFileInfo::exists(bundledClientPublicKeyPath());
    if (!hasBundledSharedKey && !QFileInfo::exists(clientKeyPath())) {
        if (!runTool(sshKeygenExePath(), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(), QStringLiteral("-f"), clientKeyPath()}, 15000, errorMessage)) {
            return false;
        }
    }
// =====wjy====
    if (!ensurePrivateKeyPermissions(effectiveClientKeyPath(), errorMessage)) {
        return false;
    } // wjy: 无论使用自动生成的 client_ed25519，还是随 OpenSSH 目录分发的 fsremote_client_ed25519，都要修 ACL，否则 ssh.exe 会因 bad permissions 忽略私钥。
// ===end====

    QFile publicKeyFile(effectiveClientPublicKeyPath());
    if (!publicKeyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to read client public key");
        }
        return false;
    }
    const QByteArray publicKey = publicKeyFile.readAll().trimmed();
    publicKeyFile.close();
    if (publicKey.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("client public key is empty");
        }
        return false;
    }

    QFile authorizedKeysFile(authorizedKeysPath());
    QByteArray existing;
    if (authorizedKeysFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        existing = authorizedKeysFile.readAll().trimmed();
        authorizedKeysFile.close();
    }
// =====wjy====
    if (authorizedKeysContain(existing, publicKey)) {
        return true;
    }

    QSaveFile saveFile(authorizedKeysPath());
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to write authorized_keys");
        }
        return false;
    }
    saveFile.write(appendAuthorizedKeyLine(existing, publicKey)); // wjy: 只追加本机公钥，不覆盖其它 FSRemote 设备已经登记进来的远程公钥。
    if (!saveFile.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to commit authorized_keys");
        }
        return false;
    }
    return true;
// ===end====
}

bool PortableOpenSshManager::ensureConfig(QString* errorMessage)
{
    const QString content = QStringLiteral(
        "Port %1\n"
        "ListenAddress 0.0.0.0\n"
        "HostKey %2\n"
        "AuthorizedKeysFile %3\n"
        "PasswordAuthentication no\n"
        "PubkeyAuthentication yes\n"
        "KbdInteractiveAuthentication no\n"
        "StrictModes no\n"
        "LogLevel INFO\n"
        "PidFile %4\n"
        "ForceCommand %5\n"
        "Subsystem sftp %6\n")
        .arg(serverPort())
        .arg(quoteForSshdConfigPath(hostKeyPath()))
        .arg(quoteForSshdConfigPath(authorizedKeysPath()))
        .arg(quoteForSshdConfigPath(sshdPidPath()))
        .arg(QDir::toNativeSeparators(shellPath()))
        .arg(quoteForSshdConfigPath(sftpServerExePath()));

    QFile existingFile(sshdConfigPath());
    if (existingFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString existing = QString::fromUtf8(existingFile.readAll());
        existingFile.close();
        if (existing == content) {
            return true;
        }
    }

    QSaveFile saveFile(sshdConfigPath());
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to write sshd_config");
        }
        return false;
    }
    saveFile.write(content.toUtf8());
    if (!saveFile.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to commit sshd_config");
        }
        return false;
    }
    return true;
}

bool PortableOpenSshManager::ensurePrivateKeyPermissions(const QString& keyPath, QString* errorMessage) const
{
// =====wjy====
#if !defined(_WIN32)
    Q_UNUSED(keyPath);
    Q_UNUSED(errorMessage);
    return true; // wjy: 非 Windows 平台暂不需要 icacls 修复，保持原有行为。
#else
    const QString normalizedKeyPath = QDir::toNativeSeparators(QFileInfo(keyPath).absoluteFilePath());
    const QString aclUser = currentProcessAccount();
    const QString loginUser = currentLoginUser();
    if (normalizedKeyPath.isEmpty() || aclUser.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to prepare OpenSSH key permission context");
        }
        return false; // wjy: 没有明确文件路径或登录用户时不盲目修改 ACL，避免误改其它文件。
    }

    auto runIcacls = [this, errorMessage](const QStringList& arguments) {
        return runTool(QStringLiteral("icacls.exe"), arguments, 8000, errorMessage); // wjy: 复用同步工具执行逻辑，失败时把 stderr/stdout 带回 UI。
    };

    if (!runIcacls({normalizedKeyPath, QStringLiteral("/inheritance:r")})) {
        return false; // wjy: 关闭继承，避免 Users/Everyone 等宽权限让 OpenSSH 拒绝加载私钥。
    }
    if (!runIcacls({
        normalizedKeyPath,
        QStringLiteral("/grant:r"),
        aclUser + QStringLiteral(":(F)"),
        QStringLiteral("*S-1-5-18:(F)"),
        QStringLiteral("*S-1-5-32-544:(F)"),
    })) {
        return false; // wjy: 当前进程账户、SYSTEM 和 Administrators 获得完全控制，OpenSSH 才会接受该私钥。
    }
    if (!loginUser.isEmpty() && QString::compare(loginUser, aclUser, Qt::CaseInsensitive) != 0) {
        return runIcacls({normalizedKeyPath, QStringLiteral("/remove:g"), loginUser}); // wjy: 沙箱/服务账户运行时移除环境登录用户的旧显式权限，避免 OpenSSH 判定私钥过度开放。
    }
    return true;
#endif
// ===end====
}

bool PortableOpenSshManager::cleanupResidualServerProcesses(QString* errorMessage) const
{
#if !defined(_WIN32)
    Q_UNUSED(errorMessage);
    QFile::remove(sshdPidPath());
    return true;
#else
    QFile::remove(sshdPidPath());

    const QString targetPath = QFileInfo(sshdExePath()).absoluteFilePath();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to enumerate processes (%1)").arg(GetLastError());
        }
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool ok = Process32FirstW(snapshot, &entry) == TRUE;
    while (ok) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (process) {
            wchar_t buffer[MAX_PATH * 4] = {};
            DWORD length = static_cast<DWORD>(std::size(buffer));
            if (QueryFullProcessImageNameW(process, 0, buffer, &length) == TRUE) {
                const QString processPath = QDir::toNativeSeparators(QString::fromWCharArray(buffer, static_cast<int>(length)));
                if (QString::compare(processPath, QDir::toNativeSeparators(targetPath), Qt::CaseInsensitive) == 0) {
                    TerminateProcess(process, 0);
                    WaitForSingleObject(process, 2000);
                }
            }
            CloseHandle(process);
        }
        ok = Process32NextW(snapshot, &entry) == TRUE;
    }

    CloseHandle(snapshot);
    return true;
#endif
}

bool PortableOpenSshManager::runTool(const QString& program, const QStringList& arguments, int timeoutMs, QString* errorMessage) const
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(opensshDir());

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString pathKey = environment.contains(QStringLiteral("Path")) ? QStringLiteral("Path") : QStringLiteral("PATH");
    const QString pathValue = environment.value(pathKey);
    environment.insert(pathKey, opensshDir() + QLatin1Char(';') + pathValue);
    process.setProcessEnvironment(environment);

    process.start();
    if (!process.waitForStarted(2000)) {
        if (errorMessage) {
            *errorMessage = process.errorString();
        }
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        if (errorMessage) {
            *errorMessage = QStringLiteral("%1 timed out").arg(QFileInfo(program).fileName());
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorMessage) {
            QString message = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            if (message.isEmpty()) {
                message = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
            }
            *errorMessage = message.isEmpty()
                ? QStringLiteral("%1 failed with exit code %2").arg(QFileInfo(program).fileName()).arg(process.exitCode())
                : message;
        }
        return false;
    }
    return true;
}

// =====wjy====
bool PortableOpenSshManager::runToolWithStandardInput(
    const QString& program,
    const QStringList& arguments,
    const QString& inputPath,
    int timeoutMs,
    QString* errorMessage) const
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(opensshDir());
    process.setStandardInputFile(inputPath); // wjy: `ssh-keygen -Y verify` 只从标准输入接收被签名内容，避免命令行转义改变原始字节。
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString pathKey = environment.contains(QStringLiteral("Path")) ? QStringLiteral("Path") : QStringLiteral("PATH");
    environment.insert(pathKey, opensshDir() + QLatin1Char(';') + environment.value(pathKey));
    process.setProcessEnvironment(environment);
    process.start();
    if (!process.waitForStarted(2000)) {
        if (errorMessage) *errorMessage = QStringLiteral("failed to start %1").arg(QFileInfo(program).fileName());
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        if (errorMessage) *errorMessage = QStringLiteral("%1 timed out").arg(QFileInfo(program).fileName());
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorMessage) {
            QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            if (detail.isEmpty()) detail = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
            *errorMessage = detail.isEmpty() ? QStringLiteral("session signature verification failed") : detail;
        }
        return false;
    }
    if (errorMessage) errorMessage->clear();
    return true;
}
// ===end====

QString PortableOpenSshManager::appDir() const
{
    return QCoreApplication::applicationDirPath();
}

QString PortableOpenSshManager::opensshDir() const
{
    return appDir() + QStringLiteral("/openssh/OpenSSH-Win64");
}

QString PortableOpenSshManager::dataDir() const
{
    return appDir() + QStringLiteral("/data/openssh");
}

QString PortableOpenSshManager::sshExePath() const
{
    return opensshDir() + QStringLiteral("/ssh.exe");
}

QString PortableOpenSshManager::sshdExePath() const
{
    return opensshDir() + QStringLiteral("/sshd.exe");
}

QString PortableOpenSshManager::sshKeygenExePath() const
{
    return opensshDir() + QStringLiteral("/ssh-keygen.exe");
}

QString PortableOpenSshManager::sftpServerExePath() const
{
    return opensshDir() + QStringLiteral("/sftp-server.exe");
}

QString PortableOpenSshManager::bundledClientKeyPath() const
{
    return opensshDir() + QStringLiteral("/fsremote_client_ed25519");
}

QString PortableOpenSshManager::bundledClientPublicKeyPath() const
{
    return opensshDir() + QStringLiteral("/fsremote_client_ed25519.pub");
}

QString PortableOpenSshManager::effectiveClientKeyPath() const
{
    if (QFileInfo::exists(bundledClientKeyPath()) && QFileInfo::exists(bundledClientPublicKeyPath())) {
        return bundledClientKeyPath();
    }
    return clientKeyPath();
}

QString PortableOpenSshManager::effectiveClientPublicKeyPath() const
{
    if (QFileInfo::exists(bundledClientKeyPath()) && QFileInfo::exists(bundledClientPublicKeyPath())) {
        return bundledClientPublicKeyPath();
    }
    return clientKeyPath() + QStringLiteral(".pub");
}

QString PortableOpenSshManager::clientKeyPath() const
{
    return dataDir() + QStringLiteral("/client_ed25519");
}

QString PortableOpenSshManager::hostKeyPath() const
{
    return dataDir() + QStringLiteral("/ssh_host_ed25519_key");
}

QString PortableOpenSshManager::authorizedKeysPath() const
{
    return dataDir() + QStringLiteral("/authorized_keys");
}

QString PortableOpenSshManager::sshdPidPath() const
{
    return dataDir() + QStringLiteral("/sshd.pid");
}

QString PortableOpenSshManager::sshdConfigPath() const
{
    return dataDir() + QStringLiteral("/sshd_config");
}

QString PortableOpenSshManager::sshdLogPath() const
{
    return dataDir() + QStringLiteral("/sshd.log");
}

QString PortableOpenSshManager::currentLoginUser() const
{
    const QString userName = qEnvironmentVariable("USERNAME").trimmed();
    if (userName.isEmpty()) {
        return {};
    }

    const QString domain = qEnvironmentVariable("USERDOMAIN").trimmed();
    const QString computerName = qEnvironmentVariable("COMPUTERNAME").trimmed();
    if (!domain.isEmpty()
        && !computerName.isEmpty()
        && QString::compare(domain, computerName, Qt::CaseInsensitive) != 0) {
        return domain + QStringLiteral("\\") + userName;
    }
    return userName;
}

QString PortableOpenSshManager::shellPath() const
{
    return QStringLiteral("C:/Windows/System32/cmd.exe");
}

} // namespace platform
