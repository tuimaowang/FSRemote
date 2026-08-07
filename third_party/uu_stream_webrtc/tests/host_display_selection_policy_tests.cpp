#include "host_display_selection_policy.h"

#include <cassert>
#include <vector>

namespace {

// =====wjy====
uu::HostDisplayCandidate valid_primary(const char* name = "\\\\.\\DISPLAY1")
{
    uu::HostDisplayCandidate candidate;
    candidate.device_name = name; // wjy: 每个用例从同一份有效主屏基线出发，只改变要验证的排除条件。
    candidate.monitor_id = 101;
    candidate.width = 1920;
    candidate.height = 1080;
    candidate.refresh_hz = 60;
    candidate.active = true;
    candidate.primary = true;
    return candidate;
}
// ===end====

} // namespace

int main()
{
    // =====wjy====
    const auto selected = uu::select_existing_primary_display({valid_primary()});
    assert(selected.has_value());
    assert(selected->device_name == "\\\\.\\DISPLAY1"); // wjy: 合格真实主屏必须成为首选目标，不触发 VDD。

    auto parsec = valid_primary();
    parsec.parsec = true;
    assert(!uu::select_existing_primary_display({parsec})); // wjy: 现存 Parsec 屏不能冒充真实主屏。

    auto inactive = valid_primary();
    inactive.active = false;
    assert(!uu::select_existing_primary_display({inactive})); // wjy: 已断开但仍残留在枚举中的输出必须进入 VDD 兜底。

    auto secondary = valid_primary();
    secondary.primary = false;
    assert(!uu::select_existing_primary_display({secondary})); // wjy: 副屏会破坏当前绝对鼠标主屏坐标语义，因此第一版明确排除。

    auto remote = valid_primary();
    remote.remote = true;
    assert(!uu::select_existing_primary_display({remote})); // wjy: 临时远程会话输出断开后会消失，不能作为稳定 Host 目标。

    auto mirroring = valid_primary();
    mirroring.mirroring = true;
    assert(!uu::select_existing_primary_display({mirroring})); // wjy: 镜像驱动没有独立可复制桌面，不能绕过 VDD。

    auto invalid_mode = valid_primary();
    invalid_mode.width = 0;
    assert(!uu::select_existing_primary_display({invalid_mode})); // wjy: 无有效当前模式的主屏不能被捕获。

    auto missing_monitor = valid_primary();
    missing_monitor.monitor_id = 0;
    assert(!uu::select_existing_primary_display({missing_monitor})); // wjy: 缺少 HMONITOR 时无法保证 CPU 回退仍命中同一主屏。

    std::vector<uu::HostDisplayCandidate> ordered{secondary, valid_primary("\\\\.\\DISPLAY3")};
    const auto later_primary = uu::select_existing_primary_display(ordered);
    assert(later_primary && later_primary->device_name == "\\\\.\\DISPLAY3"); // wjy: 枚举前方存在副屏时仍必须找到真正主屏。
    // ===end====
    return 0;
}
