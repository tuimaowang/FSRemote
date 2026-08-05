#include "ui/RemoteClipboardCodec.h"

#include <QByteArray>

namespace ui {

// =====wjy====
QByteArray RemoteClipboardCodec::encode(const QString& text)
{
    if (text.isEmpty()) {
        return {}; // wjy: 空剪贴板不发网络消息，避免把“清空”误当成无效控制包。
    }
    const QByteArray utf8 = text.toUtf8();
    if (utf8.isEmpty() || utf8.size() > kMaxTextBytes) {
        return {}; // wjy: 限制单条文本大小，防止剪贴板同步占满控制通道或产生异常大消息。
    }
    return utf8.toBase64(); // wjy: 复用现有 cb <base64> 协议格式，不修改远端协议字段。
}

bool RemoteClipboardCodec::decode(const QString& encodedBase64, QString* text)
{
    if (!text) {
        return false;
    }
    const QByteArray decoded = QByteArray::fromBase64(encodedBase64.toLatin1()); // wjy: 回撤16:32附带的严格Base64行为，恢复该时间点之前的剪贴板兼容解码。
    if (decoded.isEmpty() || decoded.size() > kMaxTextBytes) {
        return false; // wjy: 空数据和超限数据直接丢弃，避免覆盖本地剪贴板或触发回环。
    }
    const QString decodedText = QString::fromUtf8(decoded);
    if (decodedText.isEmpty()) {
        return false; // wjy: 非法 UTF-8 或空文本不进入 UI 剪贴板。
    }
    *text = decodedText;
    return true;
}
// ===end====

} // namespace ui
