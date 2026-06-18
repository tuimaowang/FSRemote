#include "system/PortableOpenSshManager.h"

#include <QCoreApplication>
#include <QDir>
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
    std::wstring applicationName = QDir::toNativeSeparators(sshExePath()).toStdWString();
    QStringList commandParts;
    commandParts.reserve(arguments.size() + 1);
    commandParts.push_back(quoteForCmd(QDir::toNativeSeparators(sshExePath())));
    for (const QString& argument : arguments) {
        commandParts.push_back(argument.contains(QLatin1Char(' ')) ? quoteForCmd(argument) : argument);
    }
    std::wstring mutableCommandLine = commandParts.join(QLatin1Char(' ')).toStdWString();
    std::wstring workingDirectory = QDir::toNativeSeparators(appDir()).toStdWString();

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

    const bool hasBundledSharedKey =
        QFileInfo::exists(bundledClientKeyPath())
        && QFileInfo::exists(bundledClientPublicKeyPath());
    if (!hasBundledSharedKey && !QFileInfo::exists(clientKeyPath())) {
        if (!runTool(sshKeygenExePath(), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(), QStringLiteral("-f"), clientKeyPath()}, 15000, errorMessage)) {
            return false;
        }
    }

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
    if (existing == publicKey) {
        return true;
    }

    QSaveFile saveFile(authorizedKeysPath());
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to write authorized_keys");
        }
        return false;
    }
    saveFile.write(publicKey);
    saveFile.write("\n");
    if (!saveFile.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to commit authorized_keys");
        }
        return false;
    }
    return true;
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
        .arg(QDir::toNativeSeparators(hostKeyPath()))
        .arg(QDir::toNativeSeparators(authorizedKeysPath()))
        .arg(QDir::toNativeSeparators(sshdPidPath()))
        .arg(QDir::toNativeSeparators(shellPath()))
        .arg(QDir::toNativeSeparators(sftpServerExePath()));

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
