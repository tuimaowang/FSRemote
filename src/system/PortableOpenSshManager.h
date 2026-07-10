#pragma once

#include <functional>

#include <QString>
#include <QStringList>

class QProcess;

namespace platform {

class PortableOpenSshManager final {
public:
    static PortableOpenSshManager& instance();

    bool startServer(QString* errorMessage = nullptr);
    void stopServer();

    bool openTerminal(const QString& hostIp, const QString& loginUser, QString* errorMessage = nullptr);
    bool runRemoteCommands(const QString& hostIp, const QString& loginUser, const QStringList& commands, QString* outputText = nullptr, QString* errorMessage = nullptr, int timeoutMs = 120000, std::function<void(const QString&)> outputCallback = {}, std::function<bool()> shouldCancel = {});
    bool runRemotePowerShellScript(const QString& hostIp, const QString& loginUser, const QString& script, QString* outputText = nullptr, QString* errorMessage = nullptr, int timeoutMs = 120000, std::function<void(const QString&)> outputCallback = {}, std::function<bool()> shouldCancel = {}); // wjy: 将长 PowerShell 分块写入远端临时 ps1 后执行，避开 cmd 单行长度限制和 Base64 命令回显。
    QString clientPublicKey(QString* errorMessage = nullptr);
    bool authorizeClientPublicKey(const QString& publicKey, QString* errorMessage = nullptr);
    QString loginUser() const;
    uint16_t serverPort() const;

private:
    PortableOpenSshManager() = default;
    ~PortableOpenSshManager();

    bool ensurePrepared(QString* errorMessage);
    bool ensureLayout(QString* errorMessage) const;
    bool ensureKeys(QString* errorMessage);
    bool ensureConfig(QString* errorMessage);
    bool ensurePrivateKeyPermissions(const QString& keyPath, QString* errorMessage) const;
    bool cleanupResidualServerProcesses(QString* errorMessage) const;
    bool runTool(const QString& program, const QStringList& arguments, int timeoutMs, QString* errorMessage) const;

    QString appDir() const;
    QString opensshDir() const;
    QString dataDir() const;
    QString sshExePath() const;
    QString sshdExePath() const;
    QString sshKeygenExePath() const;
    QString sftpServerExePath() const;
    QString bundledClientKeyPath() const;
    QString bundledClientPublicKeyPath() const;
    QString effectiveClientKeyPath() const;
    QString effectiveClientPublicKeyPath() const;
    QString clientKeyPath() const;
    QString hostKeyPath() const;
    QString authorizedKeysPath() const;
    QString sshdPidPath() const;
    QString sshdConfigPath() const;
    QString sshdLogPath() const;
    QString currentLoginUser() const;
    QString shellPath() const;

    bool m_prepared = false;
    QProcess* m_sshdProcess = nullptr;
};

} // namespace platform
