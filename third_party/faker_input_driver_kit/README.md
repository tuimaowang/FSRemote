# FakerInput 驱动键鼠最小集成包

不依赖 FSRemote、Qt、WebRTC 或 `fsremote_stream.dll`，也不要求运行 `FakerInputBridge.exe`。

## 目录内容

```text
FakerInputDriverKit/
├─ CMakeLists.txt                         独立测试程序构建入口
├─ driver/
│  └─ FakerInput_Setup_0.1.1_x64.msi   驱动安装包
├─ src/
│  ├─ faker_input_device.h              驱动设备访问接口
│  ├─ faker_input_device.cpp            HID 设备枚举与报告发送实现
│  └─ faker_input_keyboard_state.h      Windows VK 到 USB HID 的映射和按键状态机
├─ test/
│  └─ main.cpp                           安全状态检查和显式键鼠测试
├─ README.md                             本说明
└─ THIRD_PARTY_NOTICES.md                FakerInput MIT 授权说明
```

## 1. 安装驱动

在目标 Windows x64 设备上，以管理员身份执行：

```powershell
msiexec.exe /i .\driver\FakerInput_Setup_0.1.1_x64.msi /qn /norestart
```

安装包 SHA-256：

```text
4C0AEFB7340051A91D606776243298B5CD1143EF5508BBAE6800C474F9ED0840
```

如果安装程序返回需要重启，应先重启 Windows，再测试驱动。

## 2. 编译和运行测试程序

在 Visual Studio 2022 x64 开发环境中执行：

```powershell
cmake.exe -S . -B build -A x64
cmake.exe --build build --config Release
```

默认命令只打开驱动并显示版本，不产生任何键鼠输入：

```powershell
.\build\Release\FakerInputDriverTest.exe --status
```

真实输入测试必须显式指定参数，并且都会先倒计时五秒：

```powershell
.\build\Release\FakerInputDriverTest.exe --key-test
.\build\Release\FakerInputDriverTest.exe --mouse-test
.\build\Release\FakerInputDriverTest.exe --absolute-mouse-test
```

- `--key-test`：发送一次 `A` 按下和抬起；
- `--mouse-test`：鼠标向右移动 50 个相对单位，停留约半秒后再移动回来，并打印起点、终点和净位移；
- `--absolute-mouse-test`：按主显示器范围换算 `0..32767`，水平移动约 200 像素并返回；若从副屏启动，结束时使用 Win32 恢复原位置；
- `--help`：只显示命令说明。

建议先运行 `--status`。只有在无风险的测试窗口中，才运行会产生真实输入的两个命令。

## 3. 集成到自己的 C++ 程序

将 `src` 中三个文件复制到自己的工程。要求：

- Windows x64；
- C++20；
- 链接 `hid.lib`、`setupapi.lib`；测试程序额外使用 `user32.lib` 读取光标诊断坐标。

CMake 示例：

```cmake
target_sources(your_program PRIVATE
    src/faker_input_device.cpp
    src/faker_input_device.h
    src/faker_input_keyboard_state.h
)

target_compile_features(your_program PRIVATE cxx_std_20)
target_compile_definitions(your_program PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(your_program PRIVATE hid setupapi)
```

交付版 `faker_input_device.h` 已经自带这两个保护定义；工程中再次声明是为了确保其它源文件先包含 `Windows.h` 时仍保持一致。

## 4. 打开驱动

```cpp
#include "faker_input_device.h"

faker_bridge::DeviceError error;
auto device = faker_bridge::FakerInputDevice::open(&error);
if (!device) {
    // error.describe() 返回失败原因。
    return false;
}
```

## 5. 封装 `sendKey(vk, down)`

驱动键盘接收的是完整 USB HID 键盘状态，不是单个 Windows VK 增量事件。可以直接复用 `faker_input_keyboard_state.h` 管理按下状态和 VK→HID 映射：

```cpp
#include "faker_input_device.h"
#include "faker_input_keyboard_state.h"

faker_bridge::DeviceError openError;
auto device = faker_bridge::FakerInputDevice::open(&openError);
if (!device) {
    return false;
}

uu::FakerInputKeyboardState keyboardState;

auto sendKey = [&](int virtualKey, bool down)
{
    const auto previousState = keyboardState;
    const auto update = keyboardState.update(virtualKey, down);

    if (update == uu::FakerInputKeyboardUpdate::Unsupported
        || update == uu::FakerInputKeyboardUpdate::Rollover) {
        return false;
    }
    if (update == uu::FakerInputKeyboardUpdate::Unchanged) {
        return true;
    }

    const auto report = keyboardState.report();
    faker_bridge::DeviceError error;
    if (!device->send_keyboard(report.modifiers, report.usages, &error)) {
        keyboardState = previousState;
        return false;
    }
    return true;
};
```

实际使用时，可以把 `device` 和 `keyboardState` 放入自己的输入类中。

例如：

```cpp
sendKey('W', true);          // W 按下
sendKey(VK_LSHIFT, true);    // 左 Shift 按下
sendKey('W', false);         // W 抬起
sendKey(VK_LSHIFT, false);   // 左 Shift 抬起
```

普通键盘最多同时表达六个非修饰键。左右 Ctrl、Shift、Alt、Windows 键通过 modifier 位表达，不占六键槽位。

## 6. 鼠标接口

```cpp
// 相对移动；buttons 是当前完整按钮掩码。
device->send_relative_mouse(buttons, dx, dy, wheel, horizontalWheel, &error);

// 绝对坐标范围为 0..32767。
device->send_absolute_mouse(buttons, x, y, wheel, &error);
```

绝对坐标不是屏幕像素。实机验证表明 FakerInput 的绝对 HID 轴映射到 Windows 主显示器 `0..SM_CXSCREEN-1`、`0..SM_CYSCREEN-1`，不能直接寻址负坐标副屏。业务程序应把主屏坐标归一化到 `0..32767`；若必须控制其它显示器，需要改用相对鼠标、系统 `SendInput` 虚拟桌面模式或其它支持多显示器绝对轴的后端。

鼠标按钮掩码：

```text
0x01 左键
0x02 右键
0x04 中键
0x08 后退键
0x10 前进键
```

按钮同样使用完整状态快照。例如左键按下发送 `buttons=0x01`，左键抬起发送 `buttons=0x00`。

## 7. 退出和异常处理

程序退出、连接中断或发送失败时，应执行：

```cpp
device->release_all(&error);
keyboardState.clear();
```

`release_all()` 只有在当前设备实例确实还按住绝对鼠标按钮时才发送绝对释放报告。纯键盘或相对鼠标程序退出时不会额外改变光标绝对位置。

建议同一时间只让一个程序持有并控制该虚拟设备，避免不同进程的完整状态快照互相覆盖。

本包只提供标准虚拟 HID 输入能力，不包含游戏兼容、反作弊绕过或安全桌面绕过逻辑。
