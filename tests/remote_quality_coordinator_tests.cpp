#include "ui/RemoteQualityCoordinator.h"

#include <cassert>

namespace {

ui::RemoteQualityWindowMetrics visibleWindow(uintptr_t windowId)
{
    ui::RemoteQualityWindowMetrics window;
    window.windowId = windowId;
    window.sourceWidth = 3840;
    window.sourceHeight = 2160;
    return window;
}

void verifyPreset(
    const ui::RemoteQualityDecision& decision,
    stream::RemoteVideoQualityPreset preset,
    stream::RemoteResolutionTier resolution,
    int width,
    int height,
    int fps,
    int bitrateKbps)
{
    assert(decision.preset == preset);
    assert(decision.resolution == resolution);
    assert(decision.targetWidth == width);
    assert(decision.targetHeight == height);
    assert(decision.targetFps == fps);
    assert(decision.maxBitrateKbps == bitrateKbps);
}

} // namespace

int main()
{
    // =====wjy====
    stream::RemoteQualityConfiguration configuration;
    ui::RemoteQualityCoordinator coordinator;

    ui::RemoteQualityWindowMetrics first = visibleWindow(1);
    first.active = true;
    ui::RemoteQualityWindowMetrics second = visibleWindow(2);
    ui::RemoteQualityWindowMetrics third = visibleWindow(3);
    auto decisions = coordinator.evaluate(configuration, {first, second, third}, 1000);
    assert(decisions.size() == 3);
    for (const ui::RemoteQualityDecision& decision : decisions) {
        verifyPreset(
            decision,
            stream::RemoteVideoQualityPreset::P540_30,
            stream::RemoteResolutionTier::P540,
            960,
            540,
            30,
            14000); // wjy: 普通窗口无论焦点和数量都使用默认540/30，不再生成后台540/25角色。
        assert(decision.priority == 100);
        assert(decision.requestRemoteProfile);
        assert(!decision.audioEnabled);
    }

    second.fullScreen = true;
    decisions = coordinator.evaluate(configuration, {second}, 1100);
    verifyPreset(
        decisions.front(),
        stream::RemoteVideoQualityPreset::P720_30,
        stream::RemoteResolutionTier::P720,
        1280,
        720,
        30,
        24000); // wjy: 没有手选的窗口进入全屏自动切到720/30。

    ui::RemoteQualityWindowMetrics monitorWindow = visibleWindow(5);
    monitorWindow.fullScreen = true;
    monitorWindow.monitorQualityPresetActive = true;
    monitorWindow.monitorQualityPreset = stream::RemoteVideoQualityPreset::P540_25;
    decisions = coordinator.evaluate(configuration, {monitorWindow}, 1150);
    verifyPreset(
        decisions.front(),
        stream::RemoteVideoQualityPreset::P540_25,
        stream::RemoteResolutionTier::P540,
        960,
        540,
        25,
        14000); // wjy: 监控模式统一画质优先于全屏自动720/30，并且修改后无需等待性能滞回。
    monitorWindow.userQualityPresetActive = true;
    monitorWindow.userQualityPreset = stream::RemoteVideoQualityPreset::P720_60;
    decisions = coordinator.evaluate(configuration, {monitorWindow}, 1151);
    assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P720_60);
    monitorWindow.userQualityPresetActive = false;
    monitorWindow.fullyOccluded = true;
    decisions = coordinator.evaluate(configuration, {monitorWindow}, 1152);
    assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P360_1); // wjy: 窗口手选仍高于监控统一档，隐藏或完全遮挡则最终使用360/1保活。

    second.userQualityPresetActive = true;
    second.userQualityPreset = stream::RemoteVideoQualityPreset::P1080_30;
    decisions = coordinator.evaluate(configuration, {second}, 1200);
    verifyPreset(
        decisions.front(),
        stream::RemoteVideoQualityPreset::P1080_30,
        stream::RemoteResolutionTier::P1080,
        1920,
        1080,
        30,
        48000); // wjy: 手选优先于全屏自动档，全屏状态不能覆盖1080/30。
    assert(decisions.front().userSelectedPreset);
    assert(decisions.front().reason == ui::RemoteQualityDegradationReason::ModePreference);

    second.softwareFallback = true;
    decisions = coordinator.evaluate(configuration, {second}, 1300);
    assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P1080_30);
    assert(decisions.front().targetFps == 30); // wjy: 软件呈现回退只作诊断，不成为用户手选之外的隐藏改档来源。

    second.fullyOccluded = true;
    decisions = coordinator.evaluate(configuration, {second}, 1400);
    verifyPreset(
        decisions.front(),
        stream::RemoteVideoQualityPreset::P360_1,
        stream::RemoteResolutionTier::P360,
        640,
        360,
        1,
        7000);
    assert(decisions.front().fullyOccluded);
    assert(decisions.front().userSelectedPreset); // wjy: 遮挡只临时覆盖实际请求，用户1080/30意图仍保留在窗口指标中。
    assert(decisions.front().reason == ui::RemoteQualityDegradationReason::FullyOccluded);

    second.fullyOccluded = false;
    decisions = coordinator.evaluate(configuration, {second}, 1401);
    assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P1080_30);
    assert(decisions.front().targetFps == 30); // wjy: 只要重新露出任意区域，下一次评估立即恢复手选档，不等待滞回计时。

    second.minimized = true;
    decisions = coordinator.evaluate(configuration, {second}, 1500);
    assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P360_1);
    assert(decisions.front().reason == ui::RemoteQualityDegradationReason::Minimized);
    second.minimized = false;

    ui::RemoteQualityWindowMetrics hidden = visibleWindow(4);
    hidden.visible = false;
    decisions = coordinator.evaluate(configuration, {hidden}, 1600);
    assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P360_1);
    assert(decisions.front().minimized); // wjy: 隐藏和最小化继续使用不断流360/1安全档。

    first.userQualityPresetActive = true;
    first.userQualityPreset = stream::RemoteVideoQualityPreset::P720_30;
    second.userQualityPresetActive = true;
    second.userQualityPreset = stream::RemoteVideoQualityPreset::P720_30;
    third.userQualityPresetActive = true;
    third.userQualityPreset = stream::RemoteVideoQualityPreset::P1080_60;
    decisions = coordinator.evaluate(configuration, {first, second, third}, 1700);
    assert(decisions[0].preset == stream::RemoteVideoQualityPreset::P720_30);
    assert(decisions[1].preset == stream::RemoteVideoQualityPreset::P720_30);
    assert(decisions[2].preset == stream::RemoteVideoQualityPreset::P1080_60);
    assert(decisions[2].targetFps == 60); // wjy: A/B/C运行中手选多个高档完全允许，协调器不执行唯一高档限制。

    bool restoredHighPresetIsOpen = false;
    const bool restoreA = stream::shouldRestoreSavedRemoteVideoQualityPreset(
        stream::RemoteVideoQualityPreset::P720_30,
        restoredHighPresetIsOpen);
    assert(restoreA);
    restoredHighPresetIsOpen = restoreA;
    const bool restoreB = stream::shouldRestoreSavedRemoteVideoQualityPreset(
        stream::RemoteVideoQualityPreset::P720_30,
        restoredHighPresetIsOpen);
    assert(!restoreB); // wjy: A、B都保存720/30且重新打开时，A先占用自动恢复名额，B本次从默认档开始。
    const bool cAlreadyOpenAtHighPreset = true;
    assert(!stream::shouldRestoreSavedRemoteVideoQualityPreset(
        stream::RemoteVideoQualityPreset::P720_30,
        cAlreadyOpenAtHighPreset)); // wjy: C已经以手选720/30打开时，之后重新打开的A、B都不能自动恢复历史高档。
    assert(stream::shouldRestoreSavedRemoteVideoQualityPreset(
        stream::RemoteVideoQualityPreset::P540_25,
        true));
    assert(stream::shouldRestoreSavedRemoteVideoQualityPreset(
        stream::RemoteVideoQualityPreset::P540_30,
        true)); // wjy: 默认及以下历史手选不占高档恢复名额，并继续保留“手选”语义。

    ui::RemotePerformanceSignalSampler sampler;
    ui::RemotePerformanceCounters firstSample;
    firstSample.sampleTimeMs = 1000;
    firstSample.framesReceived = 100;
    firstSample.framesDecoded = 90;
    firstSample.framesDropped = 10;
    firstSample.totalDecodeTimeMs = 900.0;
    assert(!sampler.sample(firstSample).valid);
    ui::RemotePerformanceCounters secondSample = firstSample;
    secondSample.sampleTimeMs = 2000;
    secondSample.framesReceived += 60;
    secondSample.framesDecoded += 48;
    secondSample.framesDropped += 12;
    secondSample.totalDecodeTimeMs += 576.0;
    secondSample.freezeCount = 1;
    const ui::RemotePerformanceSignals sampled = sampler.sample(secondSample);
    assert(sampled.valid);
    assert(sampled.averageDecodeMs == 12.0);
    assert(sampled.decoderDropRatio > 0.16 && sampled.decoderDropRatio < 0.18);
    assert(sampled.freezeCountDelta == 1); // wjy: 性能采样继续服务诊断，但不能改写任何精确档位。

    assert(ui::shouldDispatchRemoteQualityDecision(decisions.front(), true, false));
    assert(!ui::shouldDispatchRemoteQualityDecision(decisions.front(), false, false));
    assert(!ui::shouldDispatchRemoteQualityDecision(decisions.front(), true, true));
    // ===end====
    return 0;
}
