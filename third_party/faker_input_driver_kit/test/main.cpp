#include "faker_input_device.h"
#include "faker_input_keyboard_state.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

// =====wjy====
constexpr int kExitSuccess = 0;
constexpr int kExitUsage = 1;
constexpr int kExitDriverUnavailable = 2;
constexpr int kExitInputFailed = 3; //  固定退出码便于在脚本或 CI 中区分参数、驱动和报告发送故障。
constexpr std::int16_t kMouseTestDelta = 50; // wjy: 使用明显可见但仍受控的相对位移，便于肉眼确认虚拟鼠标报告生效。
constexpr DWORD kMouseTestPauseMs = 500; // wjy: 在右移和返回之间停留半秒，让使用者能够观察到中间位置。
constexpr int kAbsoluteMouseTestPixels = 200; // wjy: 绝对测试在虚拟桌面上水平移动约 200 像素，既明显可见又避免跨越过大区域。

void printUsage()
{
    std::wcout
        << L"FakerInputDriverTest\n\n"
        << L"Usage:\n"
        << L"  FakerInputDriverTest.exe --status      Open driver only; no input is generated.\n"
        << L"  FakerInputDriverTest.exe --key-test    After 5 seconds, press and release A.\n"
        << L"  FakerInputDriverTest.exe --mouse-test  After 5 seconds, move right 50 units and back.\n"
        << L"  FakerInputDriverTest.exe --absolute-mouse-test  Move about 200px by absolute coordinates and back.\n"
        << L"  FakerInputDriverTest.exe --help        Show this help.\n"; //  帮助文本明确标出哪些命令会产生输入，避免误双击后触发键鼠操作。
}

void printDeviceError(std::wstring_view action, const faker_bridge::DeviceError& error)
{
    std::wcerr << L"ERROR: " << action << L" failed: " << error.describe() << L"\n"; //  保留 Win32 错误码与上下文，便于区分未安装、未重启和设备权限问题。
}

void countdown(std::wstring_view action)
{
    std::wcout << L"WARNING: " << action << L" will generate virtual HID input.\n";
    for (int remaining = 5; remaining > 0; --remaining) {
        std::wcout << L"Starting in " << remaining << L"...\n";
        ::Sleep(1000); //  给使用者五秒切换到无风险测试窗口，禁止参数触发后立即向当前焦点写入按键。
    }
}

bool sendKey(
    faker_bridge::FakerInputDevice& device,
    uu::FakerInputKeyboardState& keyboardState,
    int virtualKey,
    bool down)
{
    const uu::FakerInputKeyboardState previousState = keyboardState; //  报告写入失败时恢复本地镜像，后续抬键不会基于未真正送达的状态计算。
    const uu::FakerInputKeyboardUpdate update = keyboardState.update(virtualKey, down);
    if (update == uu::FakerInputKeyboardUpdate::Unsupported) {
        std::wcerr << L"ERROR: unsupported virtual key " << virtualKey << L".\n";
        return false;
    }
    if (update == uu::FakerInputKeyboardUpdate::Rollover) {
        std::wcerr << L"ERROR: keyboard report exceeded six ordinary keys.\n";
        return false;
    }
    if (update == uu::FakerInputKeyboardUpdate::Unchanged) {
        return true; //  重复 down 或不存在的 up 不产生重复 HID 快照，保持状态机幂等。
    }

    const uu::FakerInputKeyboardReport report = keyboardState.report();
    faker_bridge::DeviceError error;
    if (!device.send_keyboard(report.modifiers, report.usages, &error)) {
        keyboardState = previousState; //  驱动拒绝报告时回滚本次逻辑边沿，调用方可以修复连接后安全重试。
        printDeviceError(L"send keyboard report", error);
        return false;
    }
    return true;
}

// =====wjy====
struct PrimaryScreenGeometry {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

bool queryPrimaryScreenGeometry(PrimaryScreenGeometry* geometry)
{
    if (!geometry) return false;
    geometry->left = 0; // wjy: 实机验证确认 FakerInput 绝对 HID 轴固定映射 Windows 主屏，主屏坐标原点为 (0,0)。
    geometry->top = 0;
    geometry->width = ::GetSystemMetrics(SM_CXSCREEN);
    geometry->height = ::GetSystemMetrics(SM_CYSCREEN);
    return geometry->width > 1 && geometry->height > 1; // wjy: 主屏必须具备可归一化的有效宽高，异常指标直接拒绝测试。
}

std::uint16_t normalizeAbsoluteCoordinate(int value, int origin, int extent)
{
    const int maximum = origin + extent - 1;
    const int clamped = std::clamp(value, origin, maximum); // wjy: 屏幕边缘和负坐标统一夹紧在当前虚拟桌面范围内。
    const std::int64_t offset = static_cast<std::int64_t>(clamped) - origin;
    return static_cast<std::uint16_t>(
        (offset * 32767) / static_cast<std::int64_t>(extent - 1)); // wjy: 使用 64 位整数映射到 FakerInput 绝对轴 0..32767，避免宽屏乘法溢出。
}

bool pointInsidePrimaryScreen(const POINT& point, const PrimaryScreenGeometry& geometry)
{
    return point.x >= geometry.left
        && point.y >= geometry.top
        && point.x < geometry.left + geometry.width
        && point.y < geometry.top + geometry.height; // wjy: 负坐标副屏起点不能由 FakerInput 绝对轴直接表达，必须走本地恢复兜底。
}

POINT absoluteMouseTestTarget(const POINT& reference, const PrimaryScreenGeometry& geometry)
{
    const LONG left = static_cast<LONG>(geometry.left); // wjy: 与 Win32 POINT 坐标统一使用 LONG，避免模板比较时出现 int/LONG 类型歧义。
    const LONG right = static_cast<LONG>(geometry.left + geometry.width - 1);
    const LONG testPixels = static_cast<LONG>(kAbsoluteMouseTestPixels);
    POINT target = reference;
    if (reference.x + testPixels <= right) {
        target.x = reference.x + testPixels; // wjy: 右侧空间足够时按直观方向移动，便于用户观察。
    } else {
        target.x = std::max(left, reference.x - testPixels); // wjy: 接近右边缘时改为向左，始终把测试点留在主屏内。
    }
    target.y = std::clamp(
        reference.y,
        static_cast<LONG>(geometry.top),
        static_cast<LONG>(geometry.top + geometry.height - 1)); // wjy: 绝对目标的 Y 坐标始终限制在主屏可表达范围。
    return target;
}
// ===end====

bool releaseAll(
    faker_bridge::FakerInputDevice& device,
    uu::FakerInputKeyboardState& keyboardState)
{
    faker_bridge::DeviceError error;
    const bool released = device.release_all(&error); //  把驱动三类全零报告的真实结果返回测试入口，禁止清理失败后仍显示测试成功。
    if (!released) {
        printDeviceError(L"release all input", error); //  即使清零报告失败也输出诊断，并继续清理进程内状态避免下一次测试继承旧按键。
    }
    keyboardState.clear();
    return released;
}

int runKeyTest(
    faker_bridge::FakerInputDevice& device,
    uu::FakerInputKeyboardState& keyboardState)
{
    countdown(L"keyboard test");
    if (!sendKey(device, keyboardState, 'A', true)) {
        releaseAll(device, keyboardState);
        return kExitInputFailed;
    }
    ::Sleep(80); //  保持完整而短暂的物理按键边沿，目标 Windows 能稳定观察到 A 的按下与抬起。
    if (!sendKey(device, keyboardState, 'A', false)) {
        releaseAll(device, keyboardState);
        return kExitInputFailed;
    }
    if (!releaseAll(device, keyboardState)) {
        return kExitInputFailed; //  正常按键边沿结束后仍要求全零兜底成功，测试结果才可判定为完整通过。
    }
    std::wcout << L"RESULT: A key press/release report sent successfully.\n";
    return kExitSuccess;
}

int runMouseTest(
    faker_bridge::FakerInputDevice& device,
    uu::FakerInputKeyboardState& keyboardState)
{
    countdown(L"mouse test");
    // =====wjy====
    POINT startPosition{};
    const bool hasStartPosition = ::GetCursorPos(&startPosition); // wjy: 倒计时结束后再记录起点，排除使用者切换测试窗口产生的人工移动。
    // ===end====
    faker_bridge::DeviceError error;
    if (!device.send_relative_mouse(0, kMouseTestDelta, 0, 0, 0, &error)) {
        printDeviceError(L"move mouse right", error);
        releaseAll(device, keyboardState);
        return kExitInputFailed;
    }
    ::Sleep(kMouseTestPauseMs); // wjy: 半秒停留只用于测试可见性，不改变最终往返净位移语义。
    if (!device.send_relative_mouse(0, -kMouseTestDelta, 0, 0, 0, &error)) {
        printDeviceError(L"move mouse back", error);
        releaseAll(device, keyboardState);
        return kExitInputFailed;
    }
    if (!releaseAll(device, keyboardState)) {
        return kExitInputFailed; //  往返移动成功但最终全零报告失败时仍返回错误，避免隐藏潜在卡键鼠风险。
    }
    // =====wjy====
    POINT endPosition{};
    if (hasStartPosition && ::GetCursorPos(&endPosition)) {
        std::wcout << L"CURSOR: start=(" << startPosition.x << L"," << startPosition.y
                   << L") end=(" << endPosition.x << L"," << endPosition.y
                   << L") net=(" << (endPosition.x - startPosition.x)
                   << L"," << (endPosition.y - startPosition.y) << L")\n"; // wjy: 输出真实桌面净位移，直接区分相对移动加速与异常绝对坐标跳转。
    }
    // ===end====
    std::wcout << L"RESULT: Relative mouse round-trip reports sent successfully.\n";
    return kExitSuccess;
}

int runAbsoluteMouseTest(
    faker_bridge::FakerInputDevice& device,
    uu::FakerInputKeyboardState& keyboardState)
{
    // =====wjy====
    countdown(L"absolute mouse test");
    POINT startPosition{};
    if (!::GetCursorPos(&startPosition)) {
        std::wcerr << L"ERROR: cannot read current cursor position. Windows error=" << ::GetLastError() << L"\n";
        return kExitInputFailed;
    }

    PrimaryScreenGeometry geometry;
    if (!queryPrimaryScreenGeometry(&geometry)) {
        std::wcerr << L"ERROR: invalid primary screen geometry.\n";
        return kExitInputFailed;
    }
    const bool startOnPrimary = pointInsidePrimaryScreen(startPosition, geometry); // wjy: 副屏负坐标无法通过绝对 HID 返回，测试结束时需要 SetCursorPos 恢复原点。
    POINT referencePosition = startPosition;
    if (!startOnPrimary) {
        referencePosition.x = geometry.left + geometry.width / 2; // wjy: 副屏启动时用主屏中心作为驱动往返基准，禁止把负坐标错误夹成主屏边缘。
        referencePosition.y = geometry.top + geometry.height / 2;
    }
    const POINT targetPosition = absoluteMouseTestTarget(referencePosition, geometry);
    const std::uint16_t referenceX = normalizeAbsoluteCoordinate(referencePosition.x, geometry.left, geometry.width);
    const std::uint16_t referenceY = normalizeAbsoluteCoordinate(referencePosition.y, geometry.top, geometry.height);
    const std::uint16_t targetX = normalizeAbsoluteCoordinate(targetPosition.x, geometry.left, geometry.width);
    const std::uint16_t targetY = normalizeAbsoluteCoordinate(targetPosition.y, geometry.top, geometry.height);

    std::wcout << L"ABSOLUTE: primary=(" << geometry.left << L"," << geometry.top
               << L"," << geometry.width << L"," << geometry.height
               << L") startScreen=(" << startPosition.x << L"," << startPosition.y
               << L") startOnPrimary=" << (startOnPrimary ? L"yes" : L"no")
               << L" referenceScreen=(" << referencePosition.x << L"," << referencePosition.y
               << L") targetScreen=(" << targetPosition.x << L"," << targetPosition.y
               << L") referenceHid=(" << referenceX << L"," << referenceY
               << L") targetHid=(" << targetX << L"," << targetY << L")\n"; // wjy: 同时输出屏幕与 HID 坐标，便于定位驱动到底采用主屏还是虚拟桌面映射。

    faker_bridge::DeviceError error;
    if (!device.send_absolute_mouse(0, targetX, targetY, 0, &error)) {
        printDeviceError(L"move absolute mouse to target", error);
        releaseAll(device, keyboardState);
        return kExitInputFailed;
    }
    ::Sleep(kMouseTestPauseMs); // wjy: 在绝对目标点停留半秒，使测试移动可以被清楚观察。
    if (!device.send_absolute_mouse(0, referenceX, referenceY, 0, &error)) {
        printDeviceError(L"restore absolute mouse reference", error);
        releaseAll(device, keyboardState);
        return kExitInputFailed;
    }
    if (!releaseAll(device, keyboardState)) {
        return kExitInputFailed;
    }

    if (!startOnPrimary) {
        if (!::SetCursorPos(startPosition.x, startPosition.y)) {
            std::wcerr << L"ERROR: Win32 restore to secondary screen failed. Windows error=" << ::GetLastError() << L"\n";
            return kExitInputFailed;
        }
        std::wcout << L"RESTORE: Win32 SetCursorPos restored the secondary-screen start; FakerInput absolute HID addresses the primary screen only.\n"; // wjy: 明确区分驱动绝对报告与本地安全恢复，禁止误宣称副屏可由 HID 绝对轴寻址。
    }

    POINT endPosition{};
    if (::GetCursorPos(&endPosition)) {
        std::wcout << L"CURSOR: start=(" << startPosition.x << L"," << startPosition.y
                   << L") reference=(" << referencePosition.x << L"," << referencePosition.y
                   << L") target=(" << targetPosition.x << L"," << targetPosition.y
                   << L") end=(" << endPosition.x << L"," << endPosition.y
                   << L") net=(" << (endPosition.x - startPosition.x)
                   << L"," << (endPosition.y - startPosition.y) << L")\n"; // wjy: 绝对往返结束后报告净位移，便于确认原位置是否准确恢复。
    }
    std::wcout << L"RESULT: Absolute mouse target/restore reports sent successfully.\n";
    return kExitSuccess;
    // ===end====
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // =====wjy====
    const std::wstring_view command = argc > 1 ? argv[1] : L"--status"; //  无参数默认只探测设备，双击测试程序不会产生任何键盘或鼠标输入。
    if (argc > 2 || (command != L"--status" && command != L"--key-test"
            && command != L"--mouse-test" && command != L"--absolute-mouse-test"
            && command != L"--help")) {
        printUsage();
        return kExitUsage;
    }
    if (command == L"--help") {
        printUsage();
        return kExitSuccess;
    }

    faker_bridge::DeviceError openError;
    auto device = faker_bridge::FakerInputDevice::open(&openError); //  每次测试只打开一次 HID 控制集合，所有报告在同一设备会话中保持有序。
    if (!device) {
        printDeviceError(L"open FakerInput driver", openError);
        return kExitDriverUnavailable;
    }

    std::wcout << L"RESULT: FakerInput driver opened. Version="
               << device->driver_version() << L"\n";
    if (command == L"--status") {
        return kExitSuccess; //  状态命令在成功打开设备后立即退出，不调用任何键盘或鼠标发送接口。
    }

    uu::FakerInputKeyboardState keyboardState;
    if (command == L"--key-test") return runKeyTest(*device, keyboardState);
    if (command == L"--mouse-test") return runMouseTest(*device, keyboardState);
    return runAbsoluteMouseTest(*device, keyboardState); // wjy: 只有显式绝对测试参数能够进入 0..32767 坐标报告路径。
    // ===end====
}
