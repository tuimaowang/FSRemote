#pragma once

#include <cstdint>
#include <string>

namespace lsp {

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;

    bool valid() const { return width > 0 && height > 0; }
    bool operator==(const Size&) const = default;
};

std::string win32_error(const char* what, long hr);

} // namespace lsp
