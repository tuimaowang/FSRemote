#pragma once

// =====wjy====
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace uu {
namespace faker_input_bridge_detail {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\FakerInputBridge.v1"; // wjy: 只连接已验证的本机命名管道，不新增任何 TCP/UDP 监听面。
constexpr std::uint32_t kMagic = 0x31424946; // wjy: 与独立桥接程序 v1 的 FIB1 线格式保持一致。
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint16_t kResponseBit = 0x8000;
constexpr std::uint16_t kCommandPing = 1;
constexpr std::uint16_t kCommandKeyboard = 2; // wjy: Bridge v1 已公开键盘命令，FSRemote 现在复用同一受限本机协议发送 HID 快照。
constexpr std::uint16_t kCommandRelativeMouse = 3;
constexpr std::uint16_t kCommandAbsoluteMouse = 4;
constexpr std::uint16_t kCommandReleaseAll = 5;
constexpr std::uint32_t kDriverReadyFlag = 1u << 0;

#pragma pack(push, 1)
struct MessageHeader {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kProtocolVersion;
    std::uint16_t command = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t sequence = 0;
};

struct KeyboardPayload {
    std::uint8_t modifiers = 0;
    std::uint8_t reserved = 0;
    std::uint8_t usages[6]{};
}; // wjy: 线格式必须与独立 FakerInputBridge 的 KeyboardPayload 保持逐字节一致。

struct RelativeMousePayload {
    std::uint8_t buttons = 0;
    std::int8_t wheel = 0;
    std::int8_t horizontalWheel = 0;
    std::uint8_t reserved = 0;
    std::int16_t dx = 0;
    std::int16_t dy = 0;
};

struct AbsoluteMousePayload {
    std::uint8_t buttons = 0;
    std::int8_t wheel = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t reserved = 0;
};

struct ResponsePayload {
    std::uint32_t result = 0;
    std::uint32_t serverFlags = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 16); // wjy: 编译期锁定桥接协议布局，禁止结构体对齐变化静默破坏运行时输入。
static_assert(sizeof(KeyboardPayload) == 8); // wjy: 键盘报告严格使用 1 字节修饰键、1 字节保留和 6 个 usage。
static_assert(sizeof(RelativeMousePayload) == 8);
static_assert(sizeof(AbsoluteMousePayload) == 8);
static_assert(sizeof(ResponsePayload) == 16);

inline std::string windowsErrorText(const char* context, DWORD code)
{
    std::ostringstream text;
    text << context << " Windows error " << (code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code);
    return text.str(); // wjy: 使用 ASCII 数字错误，避免被控端本地化文本进入原生日志后出现编码失败。
}

} // namespace faker_input_bridge_detail

class FakerInputBridgeClient final {
public:
    ~FakerInputBridgeClient()
    {
        close();
    }

    FakerInputBridgeClient() = default;
    FakerInputBridgeClient(const FakerInputBridgeClient&) = delete;
    FakerInputBridgeClient& operator=(const FakerInputBridgeClient&) = delete;

    bool connectAndPing(std::string* error)
    {
        using namespace faker_input_bridge_detail;
        if (!connected()) {
            if (!::WaitNamedPipeW(kPipeName, 500)) {
                if (error) *error = windowsErrorText("wait FakerInputBridge", ::GetLastError());
                return false; // wjy: 本机桥接未启动时最多等待半秒，随后让 Host 明确回退系统鼠标。
            }
            pipe_ = ::CreateFileW(
                kPipeName,
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (!connected()) {
                if (error) *error = windowsErrorText("connect FakerInputBridge", ::GetLastError());
                return false;
            }
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!::SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr)) {
                const DWORD code = ::GetLastError();
                close();
                if (error) *error = windowsErrorText("set FakerInputBridge message mode", code);
                return false;
            }
        }
        return request(kCommandPing, nullptr, 0, error); // wjy: 已有连接也重新 ping，只有服务端和驱动仍就绪才确认切换成功。
    }

    bool sendKeyboard(
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6>& usages,
        std::string* error)
    {
        faker_input_bridge_detail::KeyboardPayload payload;
        payload.modifiers = modifiers; // wjy: 每次发送完整修饰键状态，不依赖 Bridge 推测上一个按键事件。
        std::memcpy(payload.usages, usages.data(), usages.size()); // wjy: 六键数组包含尾部零值，抬键时能明确清除已释放槽位。
        return request(
            faker_input_bridge_detail::kCommandKeyboard,
            &payload,
            sizeof(payload),
            error);
    }

    bool sendRelativeMouse(
        std::uint8_t buttons,
        std::int16_t dx,
        std::int16_t dy,
        std::int8_t wheel,
        std::int8_t horizontalWheel,
        std::string* error)
    {
        faker_input_bridge_detail::RelativeMousePayload payload;
        payload.buttons = buttons;
        payload.wheel = wheel;
        payload.horizontalWheel = horizontalWheel;
        payload.dx = dx;
        payload.dy = dy;
        return request(
            faker_input_bridge_detail::kCommandRelativeMouse,
            &payload,
            sizeof(payload),
            error); // wjy: 发送完整状态快照，而不是依赖桥接端猜测上一次按钮状态。
    }

    bool sendAbsoluteMouse(
        std::uint8_t buttons,
        std::uint16_t x,
        std::uint16_t y,
        std::int8_t wheel,
        std::string* error)
    {
        faker_input_bridge_detail::AbsoluteMousePayload payload;
        payload.buttons = buttons;
        payload.wheel = wheel;
        payload.x = x;
        payload.y = y;
        return request(faker_input_bridge_detail::kCommandAbsoluteMouse, &payload, sizeof(payload), error);
    }

    bool releaseAll(std::string* error)
    {
        if (!connected()) return true; // wjy: 从未启用桥接时没有虚拟 HID 状态需要释放，保持关闭流程幂等。
        return request(faker_input_bridge_detail::kCommandReleaseAll, nullptr, 0, error);
    }

    void close() noexcept
    {
        if (connected()) {
            ::CloseHandle(pipe_); // wjy: 服务端把断管视为安全屏障，并会再次执行 release_all 兜底。
            pipe_ = INVALID_HANDLE_VALUE;
        }
        nextSequence_ = 1;
    }

    bool connected() const noexcept
    {
        return pipe_ != INVALID_HANDLE_VALUE;
    }

private:
    bool request(
        std::uint16_t command,
        const void* payload,
        std::uint32_t payloadBytes,
        std::string* error)
    {
        using namespace faker_input_bridge_detail;
        if (!connected()) {
            if (error) *error = "FakerInputBridge is not connected";
            return false;
        }
        if ((payloadBytes > 0 && !payload) || payloadBytes > 64) {
            if (error) *error = "invalid FakerInputBridge payload";
            return false;
        }

        MessageHeader header;
        header.command = command;
        header.payloadBytes = payloadBytes;
        header.sequence = nextSequence_++;
        std::vector<std::byte> requestBytes(sizeof(header) + payloadBytes);
        std::memcpy(requestBytes.data(), &header, sizeof(header));
        if (payloadBytes > 0) {
            std::memcpy(requestBytes.data() + sizeof(header), payload, payloadBytes);
        }

        DWORD written = 0;
        if (!::WriteFile(pipe_, requestBytes.data(), static_cast<DWORD>(requestBytes.size()), &written, nullptr)
            || written != requestBytes.size()) {
            const DWORD code = ::GetLastError();
            close();
            if (error) *error = windowsErrorText("write FakerInputBridge", code);
            return false;
        }

        std::array<std::byte, sizeof(MessageHeader) + sizeof(ResponsePayload)> responseBytes{};
        DWORD read = 0;
        if (!::ReadFile(pipe_, responseBytes.data(), static_cast<DWORD>(responseBytes.size()), &read, nullptr)
            || read != responseBytes.size()) {
            const DWORD code = ::GetLastError();
            close();
            if (error) *error = windowsErrorText("read FakerInputBridge", code);
            return false;
        }

        MessageHeader responseHeader;
        ResponsePayload response;
        std::memcpy(&responseHeader, responseBytes.data(), sizeof(responseHeader));
        std::memcpy(&response, responseBytes.data() + sizeof(responseHeader), sizeof(response));
        const bool valid = responseHeader.magic == kMagic
            && responseHeader.version == kProtocolVersion
            && responseHeader.command == (command | kResponseBit)
            && responseHeader.payloadBytes == sizeof(ResponsePayload)
            && responseHeader.sequence == header.sequence;
        if (!valid || response.result != ERROR_SUCCESS
            || (command == kCommandPing && (response.serverFlags & kDriverReadyFlag) == 0)) {
            const DWORD code = valid && response.result != ERROR_SUCCESS ? response.result : ERROR_INVALID_DATA;
            close();
            if (error) *error = windowsErrorText("reject FakerInputBridge response", code);
            return false; // wjy: 错序、错版本、驱动未就绪或服务端拒绝都关闭管道，禁止继续向未知状态写输入。
        }
        return true;
    }

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::uint32_t nextSequence_ = 1;
};

} // namespace uu
// ===end====
