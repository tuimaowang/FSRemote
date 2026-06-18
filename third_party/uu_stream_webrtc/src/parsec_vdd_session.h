#pragma once

#include <stdint.h>

#include <memory>
#include <string>

namespace uu {

class ParsecVddSession final {
public:
    ParsecVddSession();
    ~ParsecVddSession();

    ParsecVddSession(const ParsecVddSession&) = delete;
    ParsecVddSession& operator=(const ParsecVddSession&) = delete;

    bool start(std::string* error);
    void stop();

    bool active() const;
    int64_t preferred_source_id() const;
    const std::string& preferred_device_name() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uu
