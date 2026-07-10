#include "system/PortableOpenSshManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace platform {
namespace {

constexpr uint16_t kPortableSshPort = 49103;

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
    stopServer();
}

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
        return false;
    }

    m_sshdProcess = process;
    return true;
}

void PortableOpenSshManager::stopServer()
{
    if (!m_sshdProcess) {
        return;
    }

    m_sshdProcess->terminate();
    if (!m_sshdProcess->waitForFinished(1200)) {
        m_sshdProcess->kill();
        m_sshdProcess->waitForFinished(1200);
    }
    delete m_sshdProcess;
    m_sshdProcess = nullptr;
}

bool PortableOpenSshManager::openTerminal(const QString& hostIp, const QString& loginUser, QString* errorMessage)
{
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
        CREATE_NEW_CONSOLE,
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

QString PortableOpenSshManager::clientPublicKey(QString* errorMessage)
{
// =====wjy====
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
        .arg(quoteForSshdConfigPath(shellPath()))
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
    const QString user = currentLoginUser();
    if (normalizedKeyPath.isEmpty() || user.isEmpty()) {
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
    return runIcacls({
        normalizedKeyPath,
        QStringLiteral("/grant:r"),
        user + QStringLiteral(":(F)"),
        QStringLiteral("*S-1-5-18:(F)"),
        QStringLiteral("*S-1-5-32-544:(F)"),
    }); // wjy: 仅保留当前登录用户、SYSTEM、Administrators 的完全控制权限，匹配 Windows OpenSSH 对私钥 ACL 的要求。
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
