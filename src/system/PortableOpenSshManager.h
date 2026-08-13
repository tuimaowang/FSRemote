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
    QString clientPublicKey(QString* errorMessage = nullptr); // wjy: 主动远控前准备客户端私钥权限并返回对应公钥，失败时阻止进入必然失败的认证流程。
    QString clientPublicKeyForDeviceIdentity(QString* errorMessage = nullptr); // wjy: 在线状态只读取稳定客户端公钥，不因本机私钥 ACL 异常停止实时设备广播。
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

    bool ensurePrepared(QString* errorMessage); // wjy: 仅准备目标端 sshd 所需布局、主机密钥和配置，不检查本机主动控制使用的客户端私钥。
    bool ensureLayout(QString* errorMessage) const;
    // 准备 sshd 主机密钥；旧主机私钥属于其它账户且拒绝修改 ACL 时会安全轮换到替代密钥。
    bool ensureHostKey(QString* errorMessage);
    // 确保客户端公钥文件存在且可读；该轻量路径不会修改客户端私钥 ACL。
    bool ensureClientPublicIdentityAvailable(QString* errorMessage);
    // 准备本机主动控制使用的客户端私钥权限，并把对应公钥保留在本机授权列表中。
    bool ensureClientIdentityPrepared(QString* errorMessage);
    // 读取当前生效客户端公钥；调用方必须先选择轻量或严格准备路径。
    QString readEffectiveClientPublicKey(QString* errorMessage) const;
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

    bool m_serverPrepared = false; // wjy: 单独缓存 sshd 服务端准备结果，客户端私钥异常不能污染目标端接收远控的能力。
    bool m_clientIdentityPrepared = false; // wjy: 仅在本机主动控制或会话签名时标记客户端私钥 ACL 已完成严格检查。
    std::recursive_mutex m_identityMutex; // wjy: DLL 的主机和 Viewer 工作线程可能同时签名/验签，串行保护 OpenSSH 准备和临时工具调用。
    QProcess* m_sshdProcess = nullptr;
#if defined(_WIN32)
    void* m_serverProcessJob = nullptr; // wjy: 独立管理本机 sshd.exe；FSRemote 异常终止时 Windows 自动关闭句柄并结束 Job 内进程。
    void* m_clientProcessJob = nullptr; // wjy: 保存 Windows Job 句柄，所有由“终端”入口启动的 cmd/ssh 都归入该 Job。
#endif
};

} // namespace platform
