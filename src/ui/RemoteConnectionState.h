#pragma once

#include <QString>

namespace ui {

// =====wjy====
class RemoteConnectionState final {
public:
    static bool releasesViewerStartupAdmission(int statusCode);
    static bool acceptsRemoteInput(int statusCode);
    static QString displayText(int statusCode, const QString& detailMessage = {});
};
// ===end====

} // namespace ui
