#include "ui/RemoteClipboardCodec.h"

#include <cassert>

int main()
{
    // =====wjy====
    const QString source = QString::fromUtf8("远程剪贴板\nline-2");
    const QByteArray encoded = ui::RemoteClipboardCodec::encode(source);
    assert(!encoded.isEmpty());

    QString decoded;
    assert(ui::RemoteClipboardCodec::decode(QString::fromLatin1(encoded), &decoded));
    assert(decoded == source); // wjy: 编解码保持中文、换行和可复制文本完全一致。
    assert(ui::RemoteClipboardCodec::encode(QString()).isEmpty());

    QString unchanged = QStringLiteral("keep");
    assert(!ui::RemoteClipboardCodec::decode(QStringLiteral("not-base64"), &unchanged));
    assert(unchanged == QStringLiteral("keep")); // wjy: 无效 payload 不覆盖调用方现有文本。
    assert(ui::RemoteClipboardCodec::encode(QString(600000, QChar('x'))).isEmpty()); // wjy: 超过硬上限的本地文本不进入远程控制通道。
    // ===end====
    return 0;
}
