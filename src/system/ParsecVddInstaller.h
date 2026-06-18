#pragma once

#include <QString>

namespace platform {

class ParsecVddInstaller final {
public:
    static bool isInstalled();
    static bool ensureInstalled(QString* errorMessage = nullptr);
    static QString installerPath();
};

} // namespace platform
