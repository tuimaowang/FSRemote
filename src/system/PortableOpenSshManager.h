#pragma once

#include <QString>

class QProcess;

namespace platform {

class PortableOpenSshManager final {
public:
    static PortableOpenSshManager& instance();

    bool startServer(QString* errorMessage = nullptr);
    void stopServer();

    bool openTerminal(const QString& hostIp, const QString& loginUser, QString* errorMessage = nullptr);
    QString loginUser() const;
    uint16_t serverPort() const;

private:
    PortableOpenSshManager() = default;
    ~PortableOpenSshManager();

    bool ensurePrepared(QString* errorMessage);
    bool ensureLayout(QString* errorMessage) const;
    bool ensureKeys(QString* errorMessage);
    bool ensureConfig(QString* errorMessage);
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
