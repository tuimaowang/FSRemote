#pragma once

#include <QByteArray>
#include <QString>

namespace ui {

// =====wjy====
class RemoteClipboardCodec final {
public:
    static constexpr int kMaxTextBytes = 512 * 1024;

    static QByteArray encode(const QString& text);
    static bool decode(const QString& encodedBase64, QString* text);
};
// ===end====

} // namespace ui
