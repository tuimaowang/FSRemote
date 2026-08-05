// =====wjy====
#include "control_admission_policy.h"
#include "faker_input_keyboard_state.h" // wjy: 纯状态测试验证常用游戏键、左右修饰键、重复按下和六键上限，无需连接真实驱动。
#include "shared_input_state.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "shared_input_state_tests failed: " << message << '\n';
    std::exit(1); // wjy: 任一多人按键语义不满足即让 CTest 明确失败，禁止带着卡键风险生成 Release。
}
} // namespace

int main()
{
    uu::SharedInputState state;
    std::atomic_bool exclusive_slot = false;
    const uu::ControlAdmissionDecision shared_a = uu::decideControlAdmission(true, true, false, &exclusive_slot);
    const uu::ControlAdmissionDecision shared_b = uu::decideControlAdmission(true, true, false, &exclusive_slot);
    require(shared_a.granted && shared_b.granted && !exclusive_slot,
        "shared policy grants every eligible controller without claiming exclusive slot"); // wjy: 直接锁定用户要求的默认多人同时控制语义。
    require(!uu::decideControlAdmission(false, true, false, &exclusive_slot).granted,
        "view-only request cannot gain control");
    const uu::ControlAdmissionDecision exclusive_a = uu::decideControlAdmission(true, true, true, &exclusive_slot);
    const uu::ControlAdmissionDecision exclusive_b = uu::decideControlAdmission(true, true, true, &exclusive_slot);
    require(exclusive_a.granted && !exclusive_b.granted,
        "explicit legacy exclusive policy still admits only one controller");
    exclusive_slot = false;

    require(state.updateKey("controller-a", 65, true) == uu::SharedInputTransition::InjectDown,
        "first holder injects key down");
    require(state.updateKey("controller-b", 65, true) == uu::SharedInputTransition::None,
        "second holder does not duplicate key down");
    require(state.updateKey("controller-a", 65, false) == uu::SharedInputTransition::None,
        "first release keeps key held by second controller");
    require(state.updateKey("controller-b", 65, false) == uu::SharedInputTransition::InjectUp,
        "last release injects key up");

    require(state.updateKey("controller-a", 66, true) == uu::SharedInputTransition::InjectDown,
        "new key injects down");
    require(state.updateKey("controller-a", 66, true) == uu::SharedInputTransition::InjectRepeat,
        "same controller preserves keyboard repeat");
    require(state.updateKey("controller-b", 66, false) == uu::SharedInputTransition::None,
        "foreign key up cannot release another controller key");

    // =====wjy====
    uu::SharedInputState backend_switch_state;
    require(backend_switch_state.updateKey("controller-a", 70, true) == uu::SharedInputTransition::InjectDown,
        "backend switch fixture holds one keyboard key");
    require(backend_switch_state.updateButton("controller-a", 1, true) == uu::SharedInputTransition::InjectDown,
        "backend switch fixture holds left mouse button");
    require(backend_switch_state.updateButton("controller-b", 2, true) == uu::SharedInputTransition::InjectDown,
        "backend switch fixture holds right mouse button");
    const std::vector<int> switched_buttons = backend_switch_state.releaseAllButtons(); // wjy: 模拟标题栏切换注入后端，必须只释放鼠标而保留键盘状态。
    require(switched_buttons.size() == 2,
        "backend switch releases every unique held mouse button exactly once");
    require(backend_switch_state.updateButton("controller-a", 1, false) == uu::SharedInputTransition::None,
        "button up after backend switch cannot release stale state");
    require(backend_switch_state.updateKey("controller-a", 70, false) == uu::SharedInputTransition::InjectUp,
        "backend switch keeps keyboard ownership intact");
    require(backend_switch_state.empty(), "backend switch fixture is fully released");

    uu::SharedInputState unified_backend_switch_state;
    require(unified_backend_switch_state.updateKey("controller-a", 'F', true) == uu::SharedInputTransition::InjectDown,
        "unified backend switch fixture holds keyboard key");
    require(unified_backend_switch_state.updateButton("controller-a", 1, true) == uu::SharedInputTransition::InjectDown,
        "unified backend switch fixture holds mouse button");
    const uu::SharedInputReleaseBatch switched_inputs = unified_backend_switch_state.releaseAll(); // wjy: 新驱动键鼠开关必须把旧后端全部键和按钮一次性释放并清空会话归属。
    require(switched_inputs.keys.size() == 1 && switched_inputs.keys.front() == 'F',
        "unified backend switch releases held keyboard keys");
    require(switched_inputs.buttons.size() == 1 && switched_inputs.buttons.front() == 1,
        "unified backend switch releases held mouse buttons");
    require(unified_backend_switch_state.empty(), "unified backend switch clears all ownership");
    // ===end====

    // =====wjy====
    require(uu::fakerInputUsageForVirtualKey('W') == 0x1A, "W maps to USB HID keyboard usage");
    require(uu::fakerInputUsageForVirtualKey(VK_SPACE) == 0x2C, "space maps to USB HID keyboard usage");
    require(uu::fakerInputModifierBit(VK_LSHIFT) == 0x02, "left shift maps to modifier bit 1");
    require(uu::fakerInputModifierBit(VK_RCONTROL) == 0x10, "right control maps to modifier bit 4");
    require(uu::fakerInputUsageForVirtualKey(VK_VOLUME_UP) == 0,
        "consumer keys remain outside keyboard collection");

    uu::FakerInputKeyboardState keyboard_state;
    require(keyboard_state.update(VK_LSHIFT, true) == uu::FakerInputKeyboardUpdate::Changed,
        "modifier down changes keyboard snapshot");
    require(keyboard_state.update('W', true) == uu::FakerInputKeyboardUpdate::Changed,
        "first W down changes keyboard snapshot");
    require(keyboard_state.update('W', true) == uu::FakerInputKeyboardUpdate::Unchanged,
        "repeated W down does not duplicate HID slot");
    require(keyboard_state.update('A', true) == uu::FakerInputKeyboardUpdate::Changed,
        "second ordinary key enters next HID slot");
    uu::FakerInputKeyboardReport keyboard_report = keyboard_state.report();
    require(keyboard_report.modifiers == 0x02
            && keyboard_report.usages[0] == 0x1A
            && keyboard_report.usages[1] == 0x04,
        "keyboard report contains modifier and ordered W/A usages"); // wjy: 锁定游戏最常用 Shift+W+A 组合的完整报告布局。

    require(keyboard_state.update('S', true) == uu::FakerInputKeyboardUpdate::Changed, "third ordinary key accepted");
    require(keyboard_state.update('D', true) == uu::FakerInputKeyboardUpdate::Changed, "fourth ordinary key accepted");
    require(keyboard_state.update('F', true) == uu::FakerInputKeyboardUpdate::Changed, "fifth ordinary key accepted");
    require(keyboard_state.update('G', true) == uu::FakerInputKeyboardUpdate::Changed, "sixth ordinary key accepted");
    require(keyboard_state.update('H', true) == uu::FakerInputKeyboardUpdate::Rollover,
        "seventh ordinary key is rejected without corrupting held six-key report");
    require(keyboard_state.update('W', false) == uu::FakerInputKeyboardUpdate::Changed,
        "held key release creates new HID snapshot");
    keyboard_report = keyboard_state.report();
    require(keyboard_report.usages[0] == 0x04 && keyboard_report.usages[5] == 0,
        "released W disappears and remaining usages stay bounded");
    keyboard_state.clear();
    keyboard_report = keyboard_state.report();
    require(keyboard_report.modifiers == 0 && keyboard_report.usages[0] == 0,
        "clear produces all-zero keyboard release report");
    // ===end====

    require(state.updateButton("controller-a", 1, true) == uu::SharedInputTransition::InjectDown,
        "first mouse holder injects down");
    require(state.updateButton("controller-b", 1, true) == uu::SharedInputTransition::None,
        "second mouse holder is merged");
    const uu::SharedInputReleaseBatch released_a = state.releaseSession("controller-a");
    require(released_a.keys.size() == 1 && released_a.keys.front() == 66,
        "disconnect releases unique held key");
    require(released_a.buttons.empty(), "disconnect keeps button held by another controller");
    const uu::SharedInputReleaseBatch released_b = state.releaseSession("controller-b");
    require(released_b.buttons.size() == 1 && released_b.buttons.front() == 1,
        "last controller disconnect releases mouse button");
    require(state.empty(), "all per-session state is removed");
    return 0;
}
// ===end====
