#pragma once

#include <functional>
#include <mutex>

#include <QByteArray>
#include <QString>
#include <QStringList>

class QProcess;

namespace platform {

class PortableOpenSshManager final {
public:
    static PortableOpenSshManager& instance();

    bool startServer(QString* errorMessage = nullptr);
    void stopServer();
    void stopClientProcesses(); // wjy: 关闭 FSRemote 自己启动的交互终端进程树，不扫描也不影响用户手动启动的 ssh.exe。

    bool openTerminal(const QString& hostIp, const QString& loginUser, QString* errorMessage = nullptr);
    bool runRemoteCommands(const QString& hostIp, const QString& loginUser, const QStringList& commands, QString* outputText = nullptr, QString* errorMessage = nullptr, int timeoutMs = 120000, std::function<void(const QString&)> outputCallback = {}, std::function<bool()> shouldCancel = {});
    bool runRemotePowerShellScript(const QString& hostIp, const QString& loginUser, const QString& script, QString* outputText = nullptr, QString* errorMessage = nullptr, int timeoutMs = 120000, std::function<void(const QString&)> outputCallback = {}, std::function<bool()> shouldCancel = {}); // wjy: 将长 PowerShell 分块写入远端临时 ps1 后执行，避开 cmd 单行长度限制和 Base64 命令回显。
    QString clientPublicKey(QString* errorMessage = nullptr);
    bool authorizeClientPublicKey(const QString& publicKey, QString* errorMessage = nullptr);
    // =====wjy====
    QByteArray signSessionChallenge(const QByteArray& challenge, QString* errorMessage = nullptr);
    bool verifySessionChallenge(const QString& publicKey, const QByteArray& challenge, const QByteArray& signature, QString* errorMessage = nullptr);
    bool isSessionPublicKeyAuthorized(const QString& publicKey, QString* errorMessage = nullptr);
    // ===end====
    QString loginUser() const;
    uint16_t serverPort() const;

private:
    PortableOpenSshManager() = default;
    ~PortableOpenSshManager();

    bool ensurePrepared(QString* errorMessage);
    bool ensureLayout(QString* errorMessage) const;
    // 准备主机密钥和客户端密钥；主机私钥属于旧账户且拒绝修改 ACL 时会安全轮换主机密钥。
    bool ensureKeys(QString* errorMessage);
    bool ensureConfig(QString* errorMessage);
    bool ensurePrivateKeyPermissions(const QString& keyPath, QString* errorMessage) const;
    // 为无法接管 ACL 的旧主机密钥创建独立替代密钥；旧文件保持不动，失败时返回完整诊断信息。
    bool createReplacementHostKeyAfterAccessDenied(const QString& permissionError, QString* errorMessage) const;
    bool cleanupResidualServerProcesses(QString* errorMessage) const;
    // =====wjy====
    bool ensureServerProcessJob(QString* errorMessage); // wjy: Windows 启动 sshd 前创建“句柄关闭即杀进程”的独立 Job，主程序被强制结束也不会遗留服务进程占用版本目录。
    bool attachServerProcessToJob(qint64 processId, QString* errorMessage); // wjy: sshd 启动成功后立即纳入受控 Job；绑定失败则禁止继续运行游离服务。
    void closeServerProcessJob(); // wjy: 主动退出时终止并关闭 sshd Job，重复调用保持幂等。
    // ===end====
    bool ensureClientProcessJob(QString* errorMessage); // wjy: Windows 终端启动前建立退出即杀子进程的 Job Object。
    bool runTool(const QString& program, const QStringList& arguments, int timeoutMs, QString* errorMessage) const;
    bool runToolWithStandardInput(const QString& program, const QStringList& arguments, const QString& inputPath, int timeoutMs, QString* errorMessage) const;

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
    // 返回最新可用的替代主机密钥；不存在替代文件时继续使用历史固定路径。
    QString hostKeyPath() const;
    QString authorizedKeysPath() const;
    QString sshdPidPath() const;
    QString sshdConfigPath() const;
    QString sshdLogPath() const;
    QString currentLoginUser() const;
    QString shellPath() const;

    bool m_prepared = false;
    std::recursive_mutex m_identityMutex; // wjy: DLL 的主机和 Viewer 工作线程可能同时签名/验签，串行保护 OpenSSH 准备和临时工具调用。
    QProcess* m_sshdProcess = nullptr;
#if defined(_WIN32)
    void* m_serverProcessJob = nullptr; // wjy: 独立管理本机 sshd.exe；FSRemote 异常终止时 Windows 自动关闭句柄并结束 Job 内进程。
    void* m_clientProcessJob = nullptr; // wjy: 保存 Windows Job 句柄，所有由“终端”入口启动的 cmd/ssh 都归入该 Job。
#endif
};

} // namespace platform
