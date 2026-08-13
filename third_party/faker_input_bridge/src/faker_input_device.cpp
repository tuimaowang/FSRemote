#include "faker_input_device.h"

#include <SetupAPI.h>
#include <hidsdi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace faker_bridge {
namespace {

constexpr std::uint16_t kVendorId = 0xFE0F;
constexpr std::uint16_t kProductId = 0x00FF;
constexpr std::uint16_t kControlUsagePage = 0xFF00;
constexpr std::uint16_t kControlUsage = 0x0001;
constexpr std::uint8_t kKeyboardReportId = 0x01;
constexpr std::uint8_t kRelativeMouseReportId = 0x03;
constexpr std::uint8_t kAbsoluteMouseReportId = 0x04;
constexpr std::uint8_t kControlReportId = 0x40;

class DeviceInfoSet final {
public:
    explicit DeviceInfoSet(HDEVINFO value) : value_(value) {}
    ~DeviceInfoSet() {
        if (value_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(value_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    [[nodiscard]] HDEVINFO get() const noexcept { return value_; }

private:
    HDEVINFO value_ = INVALID_HANDLE_VALUE;
};

class FileHandle final {
public:
    explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

void set_error(DeviceError* error, DWORD code, std::wstring context) {
    if (error != nullptr) {
        error->code = code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code;
        error->context = std::move(context);
    }
}

[[nodiscard]] HANDLE open_hid_path(const std::wstring& path, DWORD access) {
    return CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

[[nodiscard]] std::pair<std::uint16_t, std::uint16_t> current_absolute_position() {
    POINT point{};
    if (!GetCursorPos(&point)) {
        return {std::uint16_t{0}, std::uint16_t{0}};
    }

    const int width = std::max(GetSystemMetrics(SM_CXSCREEN) - 1, 1);
    const int height = std::max(GetSystemMetrics(SM_CYSCREEN) - 1, 1);
    const int x = std::clamp(static_cast<int>(point.x), 0, width);
    const int y = std::clamp(static_cast<int>(point.y), 0, height);
    return {
        static_cast<std::uint16_t>(
            (static_cast<std::uint64_t>(x) * 32767u) /
            static_cast<std::uint64_t>(width)),
        static_cast<std::uint16_t>(
            (static_cast<std::uint64_t>(y) * 32767u) /
            static_cast<std::uint64_t>(height)),
    };
}

}  // namespace

std::wstring DeviceError::describe() const {
    std::wstring result = context;
    if (code == ERROR_SUCCESS) {
        return result;
    }
    if (!result.empty()) {
        result += L": ";
    }
    result += L"Windows error ";
    result += std::to_wstring(code);

    std::wostringstream suffix;
    suffix << L" (0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill(L'0') << code << L")";
    result += suffix.str();
    return result;
}

FakerInputDevice::FakerInputDevice(
    HANDLE handle,
    std::uint16_t output_report_bytes,
    std::uint16_t driver_version)
    : handle_(handle),
      output_report_bytes_(output_report_bytes),
      driver_version_(driver_version) {}

FakerInputDevice::~FakerInputDevice() {
    close();
}

FakerInputDevice::FakerInputDevice(FakerInputDevice&& other) noexcept
    : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
      output_report_bytes_(
          std::exchange(other.output_report_bytes_, std::uint16_t{0})),
      driver_version_(std::exchange(other.driver_version_, std::uint16_t{0})) {}

FakerInputDevice& FakerInputDevice::operator=(FakerInputDevice&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        output_report_bytes_ =
            std::exchange(other.output_report_bytes_, std::uint16_t{0});
        driver_version_ =
            std::exchange(other.driver_version_, std::uint16_t{0});
    }
    return *this;
}

std::optional<FakerInputDevice> FakerInputDevice::open(DeviceError* error) {
    if (error != nullptr) {
        *error = {};
    }

    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);
    DeviceInfoSet info_set(SetupDiGetClassDevsW(
        &hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (info_set.get() == INVALID_HANDLE_VALUE) {
        set_error(error, GetLastError(), L"Enumerate HID devices");
        return std::nullopt;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(
                info_set.get(), nullptr, &hid_guid, index, &interface_data)) {
            const DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_ITEMS) {
                set_error(error, ERROR_NOT_FOUND, L"FakerInput control collection not found");
            } else {
                set_error(error, code, L"Enumerate HID interfaces");
            }
            return std::nullopt;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            info_set.get(), &interface_data, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<std::byte> detail_buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(
                info_set.get(),
                &interface_data,
                detail,
                required,
                nullptr,
                nullptr)) {
            continue;
        }

        FileHandle query_handle(open_hid_path(detail->DevicePath, 0));
        if (!query_handle.valid()) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(query_handle.get(), &attributes) ||
            attributes.VendorID != kVendorId ||
            attributes.ProductID != kProductId) {
            continue;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!HidD_GetPreparsedData(query_handle.get(), &preparsed)) {
            continue;
        }
        HIDP_CAPS caps{};
        const NTSTATUS caps_status = HidP_GetCaps(preparsed, &caps);
        HidD_FreePreparsedData(preparsed);
        if (caps_status != HIDP_STATUS_SUCCESS ||
            caps.UsagePage != kControlUsagePage || caps.Usage != kControlUsage ||
            caps.OutputReportByteLength < 65) {
            continue;
        }

        HANDLE control_handle = open_hid_path(
            detail->DevicePath, GENERIC_READ | GENERIC_WRITE);
        if (control_handle == INVALID_HANDLE_VALUE) {
            set_error(error, GetLastError(), L"Open FakerInput control collection");
            return std::nullopt;
        }

        return FakerInputDevice(
            control_handle, caps.OutputReportByteLength, attributes.VersionNumber);
    }
}

bool FakerInputDevice::send_keyboard(
    std::uint8_t modifiers,
    std::span<const std::uint8_t> usages,
    DeviceError* error) const {
    if (usages.size() > 6) {
        set_error(error, ERROR_INVALID_PARAMETER, L"At most 6 keyboard usages are allowed");
        return false;
    }

    std::array<std::uint8_t, 9> report{};
    report[0] = kKeyboardReportId;
    report[1] = modifiers;
    std::copy(usages.begin(), usages.end(), report.begin() + 3);
    return send_inner_report(report, error);
}

bool FakerInputDevice::send_relative_mouse(
    std::uint8_t buttons,
    std::int16_t dx,
    std::int16_t dy,
    std::int8_t wheel,
    std::int8_t horizontal_wheel,
    DeviceError* error) const {
    std::array<std::uint8_t, 8> report{};
    report[0] = kRelativeMouseReportId;
    report[1] = buttons;
    std::memcpy(report.data() + 2, &dx, sizeof(dx));
    std::memcpy(report.data() + 4, &dy, sizeof(dy));
    report[6] = static_cast<std::uint8_t>(wheel);
    report[7] = static_cast<std::uint8_t>(horizontal_wheel);
    return send_inner_report(report, error);
}

bool FakerInputDevice::send_absolute_mouse(
    std::uint8_t buttons,
    std::uint16_t x,
    std::uint16_t y,
    std::int8_t wheel,
    DeviceError* error) const {
    if (x > 32767 || y > 32767) {
        set_error(error, ERROR_INVALID_PARAMETER, L"Absolute coordinates must be 0..32767");
        return false;
    }

    std::array<std::uint8_t, 7> report{};
    report[0] = kAbsoluteMouseReportId;
    report[1] = buttons;
    std::memcpy(report.data() + 2, &x, sizeof(x));
    std::memcpy(report.data() + 4, &y, sizeof(y));
    report[6] = static_cast<std::uint8_t>(wheel);
    return send_inner_report(report, error);
}

bool FakerInputDevice::release_all(DeviceError* error) const {
    DeviceError first_error;
    bool success = send_keyboard(0, {}, &first_error);

    DeviceError relative_error;
    if (!send_relative_mouse(0, 0, 0, 0, 0, &relative_error) && success) {
        success = false;
        first_error = relative_error;
    }

    const auto [x, y] = current_absolute_position();
    DeviceError absolute_error;
    if (!send_absolute_mouse(0, x, y, 0, &absolute_error) && success) {
        success = false;
        first_error = absolute_error;
    }

    if (error != nullptr) {
        *error = success ? DeviceError{} : first_error;
    }
    return success;
}

std::uint16_t FakerInputDevice::driver_version() const noexcept {
    return driver_version_;
}

bool FakerInputDevice::valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
}

bool FakerInputDevice::send_inner_report(
    std::span<const std::uint8_t> report,
    DeviceError* error) const {
    if (error != nullptr) {
        *error = {};
    }
    if (!valid()) {
        set_error(error, ERROR_INVALID_HANDLE, L"FakerInput handle is not open");
        return false;
    }
    if (report.empty() || report.size() > std::numeric_limits<std::uint8_t>::max()) {
        set_error(error, ERROR_INVALID_PARAMETER, L"Invalid inner report length");
        return false;
    }
    if (output_report_bytes_ < report.size() + 2) {
        set_error(error, ERROR_INSUFFICIENT_BUFFER, L"FakerInput output report is too short");
        return false;
    }

    std::vector<std::uint8_t> outer(output_report_bytes_, 0);
    outer[0] = kControlReportId;
    outer[1] = static_cast<std::uint8_t>(report.size());
    std::copy(report.begin(), report.end(), outer.begin() + 2);
    if (HidD_SetOutputReport(
            handle_, outer.data(), static_cast<ULONG>(outer.size()))) {
        return true;
    }

    const DWORD hid_error = GetLastError();
    DWORD bytes_written = 0;
    if (WriteFile(
            handle_,
            outer.data(),
            static_cast<DWORD>(outer.size()),
            &bytes_written,
            nullptr) &&
        bytes_written == outer.size()) {
        return true;
    }

    std::wostringstream context;
    context << L"HidD_SetOutputReport failed with " << hid_error
            << L"; WriteFile fallback";
    set_error(error, GetLastError(), context.str());
    return false;
}

void FakerInputDevice::close() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

}  // namespace faker_bridge
