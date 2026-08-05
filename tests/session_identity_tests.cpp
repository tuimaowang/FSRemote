#include "system/PortableOpenSshManager.h"

#include <QCoreApplication>

#include <cstdlib>
#include <cstdio>
#include <utility>

namespace {

// =====wjy====
void require(bool condition, const QString& message, const QString& detail = {})
{
    if (condition) return;
    const QByteArray output = QStringLiteral("session_identity_tests failed: %1 %2\n").arg(message, detail).toUtf8();
    std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stderr); // wjy: CTest 在 Windows 下稳定捕获标准错误，失败步骤不再只写入调试输出器。
    std::exit(1); // wjy: 任一真实 OpenSSH 身份断言失败时立即让 CTest 获得非零退出码。
}

QByteArray replaceContextField(const QByteArray& context, const QByteArray& name, const QByteArray& value)
{
    QByteArray changed = context;
    const QByteArray prefix = name + '=';
    const qsizetype begin = changed.indexOf(prefix);
    require(begin >= 0, QStringLiteral("missing challenge field"), QString::fromLatin1(name));
    const qsizetype valueBegin = begin + prefix.size();
    qsizetype valueEnd = changed.indexOf('\n', valueBegin);
    if (valueEnd < 0) valueEnd = changed.size();
    changed.replace(valueBegin, valueEnd - valueBegin, value); // wjy: 每次只改变一个签名上下文字段，验证该字段确实参与签名绑定。
    return changed;
}
// ===end====

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    auto& manager = platform::PortableOpenSshManager::instance();

    // =====wjy====
    QString error;
    const QString publicKey = manager.clientPublicKey(&error);
    require(!publicKey.isEmpty(), QStringLiteral("read local public key"), error);
    require(manager.authorizeClientPublicKey(publicKey, &error), QStringLiteral("authorize local public key"), error);
    require(manager.isSessionPublicKeyAuthorized(publicKey, &error), QStringLiteral("exact public key must be authorized"), error);
    require(!manager.isSessionPublicKeyAuthorized(publicKey + QStringLiteral(" changed-comment"), &error),
        QStringLiteral("changed public-key line must not be authorized"));

    const QByteArray challenge =
        "FSREMOTE_SESSION_PROOF\n"
        "version=1\n"
        "client_id=client-a\n"
        "client_nonce=nonce-a\n"
        "host_id=host-a\n"
        "host_nonce=nonce-b\n"
        "requested_role=control\n";
    const QByteArray signature = manager.signSessionChallenge(challenge, &error);
    require(!signature.isEmpty(), QStringLiteral("sign challenge with OpenSSH"), error);
    require(manager.verifySessionChallenge(publicKey, challenge, signature, &error),
        QStringLiteral("verify valid OpenSSH signature"), error);

    for (const auto& mutation : {
             std::pair<QByteArray, QByteArray>{"version", "2"},
             {"client_id", "client-b"},
             {"client_nonce", "nonce-c"},
             {"host_id", "host-b"},
             {"host_nonce", "nonce-d"},
             {"requested_role", "view"},
         }) {
        const QByteArray changed = replaceContextField(challenge, mutation.first, mutation.second);
        require(!manager.verifySessionChallenge(publicKey, changed, signature, &error),
            QStringLiteral("modified challenge field must invalidate signature"), QString::fromLatin1(mutation.first));
    }

    QByteArray damagedSignature = signature;
    damagedSignature[damagedSignature.size() / 2] = damagedSignature.at(damagedSignature.size() / 2) == 'A' ? 'B' : 'A';
    require(!manager.verifySessionChallenge(publicKey, challenge, damagedSignature, &error),
        QStringLiteral("damaged signature must fail"));
    std::fputs("session_identity_tests passed\n", stdout); // wjy: 使用标准输出让命令行和 CTest 都能显示成功结果。
    // ===end====
    return 0;
}
