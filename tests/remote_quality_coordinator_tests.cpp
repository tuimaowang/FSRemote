#include "ui/RemoteQualityCoordinator.h"

#include <cassert>

int main()
{
    // =====wjy====
    stream::RemoteQualityConfiguration configuration;
    ui::RemoteQualityCoordinator coordinator;

    ui::RemoteQualityWindowMetrics activeWindow;
    activeWindow.windowId = 1;
    activeWindow.active = true;
    activeWindow.sourceWidth = 3840;
    activeWindow.sourceHeight = 2160;

    ui::RemoteQualityWindowMetrics backgroundWindow;
    backgroundWindow.windowId = 2;
    backgroundWindow.effectiveMode = stream::RemoteQualityMode::HighQualityLocked; // wjy: 旧手动模式故意设为高质，验证智能策略仍以窗口活动状态为准。

    ui::RemoteQualityWindowMetrics fullScreenWindow;
    fullScreenWindow.windowId = 3;
    fullScreenWindow.fullScreen = true;
    fullScreenWindow.effectiveMode = stream::RemoteQualityMode::Smooth;

    auto decisions = coordinator.evaluate(
        configuration,
        {activeWindow, backgroundWindow, fullScreenWindow},
        1000);
    assert(decisions.size() == 3);
    assert(decisions[0].effectiveMode == stream::RemoteQualityMode::HighQualityLocked);
    assert(decisions[0].resolution == stream::RemoteResolutionTier::P1080);
    assert(decisions[0].targetWidth == 1920 && decisions[0].targetHeight == 1080);
    assert(decisions[0].targetFps == 45); // wjy: 存在多个远控窗口时焦点窗口使用多选变量1080p/45。
    assert(!decisions[0].audioEnabled);
    assert(decisions[0].priority == 100); // wjy: 单选质量变量默认请求1080p/60并获得最高可见优先级。
    // 音频由标题栏按钮独立控制，不由质量协调器按焦点授予。

    assert(decisions[1].effectiveMode == stream::RemoteQualityMode::Smooth);
    assert(decisions[1].resolution == stream::RemoteResolutionTier::P720);
    assert(decisions[1].targetWidth == 1280 && decisions[1].targetHeight == 720);
    assert(decisions[1].targetFps == 15);
    assert(decisions[1].priority == 10);
    assert(!decisions[1].audioEnabled); // wjy: 其它普通可见窗口统一降到最低保活档且没有音频。

    assert(decisions[2].effectiveMode == stream::RemoteQualityMode::Smooth);
    assert(decisions[2].resolution == stream::RemoteResolutionTier::P720);
    assert(decisions[2].targetWidth == 1280 && decisions[2].targetHeight == 720);
    assert(decisions[2].targetFps == 15);
    assert(!decisions[2].audioEnabled); // wjy: 全屏窗口失焦时使用后台FPS，音频仍由标题栏按钮独立控制。

    activeWindow.active = false;
    backgroundWindow.active = true;
    decisions = coordinator.evaluate(configuration, {activeWindow, backgroundWindow}, 1100);
    assert(decisions[0].effectiveMode == stream::RemoteQualityMode::Smooth);
    assert(decisions[0].resolution == stream::RemoteResolutionTier::P720);
    assert(decisions[0].targetFps == 15);
    assert(!decisions[0].audioEnabled);
    assert(decisions[1].effectiveMode == stream::RemoteQualityMode::HighQualityLocked);
    assert(decisions[1].resolution == stream::RemoteResolutionTier::P1080);
    assert(decisions[1].targetFps == 45);
    assert(decisions[1].priority == 100);
    assert(!decisions[1].audioEnabled);

    backgroundWindow.minimized = true;
    decisions = coordinator.evaluate(configuration, {backgroundWindow}, 1200);
    assert(decisions.front().minimized);
    assert(decisions.front().resolution == stream::RemoteResolutionTier::P360); // wjy: 最小化窗口固定请求640x360，避免后台继续编码540p。
    assert(decisions.front().targetFps == configuration.minimizedFps);
    assert(decisions.front().priority == 10);
    assert(!decisions.front().audioEnabled);
    assert(decisions.front().reason == ui::RemoteQualityDegradationReason::Minimized); // wjy: 活动窗口一旦最小化，后台安全档优先于高质量身份。

    fullScreenWindow.active = true;
    fullScreenWindow.softwareFallback = true;
    decisions = coordinator.evaluate(configuration, {fullScreenWindow}, 1300);
    assert(decisions.front().effectiveMode == stream::RemoteQualityMode::HighQualityLocked);
    assert(decisions.front().resolution == stream::RemoteResolutionTier::P540); // wjy: 软件回退继续使用540p/24 FPS，避免把兼容路径与最小化安全档混为一谈。
    assert(decisions.front().targetFps == 24);
    assert(decisions.front().reason == ui::RemoteQualityDegradationReason::SoftwareFallback); // wjy: 全屏高质量仍受软件呈现安全档约束，避免回退路径资源失控。

    stream::RemoteQualityConfiguration thresholdConfiguration = configuration;
    thresholdConfiguration.largestWindowHighQualityMinArea = 4096 * 2160;
    activeWindow.active = true;
    decisions = coordinator.evaluate(thresholdConfiguration, {activeWindow}, 1350);
    assert(decisions.front().effectiveMode == stream::RemoteQualityMode::HighQualityLocked);
    assert(decisions.front().resolution == stream::RemoteResolutionTier::P1080);
    assert(decisions.front().targetWidth == 1920 && decisions.front().targetHeight == 1080);
    assert(decisions.front().targetFps == 60);
    assert(decisions.front().reason == ui::RemoteQualityDegradationReason::None);

    ui::RemoteQualityWindowMetrics hiddenWindow;
    hiddenWindow.windowId = 4;
    hiddenWindow.visible = false;
    hiddenWindow.active = true;
    decisions = coordinator.evaluate(configuration, {hiddenWindow}, 1400);
    assert(decisions.front().minimized);
    assert(decisions.front().resolution == stream::RemoteResolutionTier::P360); // wjy: 隐藏状态与最小化共用360p/1 FPS安全档。
    assert(decisions.front().targetFps == configuration.minimizedFps); // wjy: 隐藏状态与最小化统一使用当前最小化FPS。

    fullScreenWindow.active = false;
    fullScreenWindow.softwareFallback = false; // wjy: 清除上一段软件回退状态，单独验证正常硬件路径下单窗口无焦点仍保持高质量。
    decisions = coordinator.evaluate(configuration, {fullScreenWindow}, 1500);
    assert(decisions.front().effectiveMode == stream::RemoteQualityMode::HighQualityLocked);
    assert(decisions.front().resolution == stream::RemoteResolutionTier::P1080);
    assert(decisions.front().targetFps == 60);
    assert(decisions.front().priority == 100);
    assert(decisions.front().reason == ui::RemoteQualityDegradationReason::None);
    assert(!decisions.front().audioEnabled); // wjy: 总共只有一个可见远控窗口时即使没有焦点也保持单选质量变量，音频仍由窗口按钮独立控制。

    activeWindow.active = false;
    backgroundWindow.active = false;
    backgroundWindow.minimized = false;
    decisions = coordinator.evaluate(configuration, {activeWindow, backgroundWindow}, 1550);
    assert(decisions.size() == 2);
    assert(decisions[0].effectiveMode == stream::RemoteQualityMode::Smooth);
    assert(decisions[0].resolution == stream::RemoteResolutionTier::P720);
    assert(decisions[0].targetFps == 15);
    assert(decisions[1].effectiveMode == stream::RemoteQualityMode::Smooth);
    assert(decisions[1].resolution == stream::RemoteResolutionTier::P720);
    assert(decisions[1].targetFps == 15); // wjy: 存在两个远控窗口且都无焦点时，两者都使用多选后台720p/15。

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
    assert(sampled.freezeCountDelta == 1); // wjy: 性能采样继续服务状态展示和诊断，不再决定活动窗口的清晰档位。

    ui::RemoteQualityCoordinator highRefreshCoordinator;
    ui::RemoteQualityWindowMetrics highRefreshWindow;
    highRefreshWindow.windowId = 10;
    highRefreshWindow.active = true;
    highRefreshWindow.qualityV2Supported = true;
    highRefreshWindow.onlineFpsUpdate = true;
    highRefreshWindow.sourceRefreshHz = 144;
    highRefreshWindow.localRefreshHz = 144;
    highRefreshWindow.maxCaptureFps = 240;
    highRefreshWindow.maxEncodeFps = 240;
    highRefreshWindow.receiveFps = 60.0;
    highRefreshWindow.encodedMbps = 30.0;
    highRefreshWindow.performance.valid = true;
    highRefreshWindow.performance.averageDecodeMs = 4.0;
    highRefreshWindow.performance.averageProcessingDelayMs = 6.0;
    auto highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 1000);
    assert(highRefreshDecisions.front().targetFps == 60);
    highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 6000);
    assert(highRefreshDecisions.front().targetFps == 60); // wjy: 即使Host/显示器支持144Hz，最大窗口策略也固定为60 FPS。

    highRefreshWindow.receiveFps = 90.0;
    highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 7000);
    highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 12000);
    assert(highRefreshDecisions.front().targetFps == 60); // wjy: 健康采样不会把最大窗口升到60以上。

    highRefreshWindow.receiveFps = 100.0;
    highRefreshWindow.performance.packetLossRatio = 0.04;
    highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 13000);
    highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 15000);
    assert(highRefreshDecisions.front().targetFps == 60); // wjy: 最大窗口不因后台/统计压力改写为低于60的固定高质量请求。

    highRefreshWindow.receiveFps = 90.0;
    highRefreshWindow.performance.packetLossRatio = 0.0;
    highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 20000);
    highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 24000);
    assert(highRefreshDecisions.front().targetFps == 60); // wjy: 固定高质量没有90 FPS冷却档位。

    highRefreshWindow.receiveFps = 40.0;
    highRefreshWindow.performance.packetLossRatio = 0.10;
    highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {highRefreshWindow}, 25000);
    assert(highRefreshDecisions.front().targetFps == 60); // 严重压力直接回60并进入冷却，不继续向下破坏高质量语义。

    ui::RemoteQualityWindowMetrics legacyWindow = highRefreshWindow;
    legacyWindow.windowId = 11;
    legacyWindow.qualityV2Supported = false;
    legacyWindow.onlineFpsUpdate = false;
    legacyWindow.receiveFps = 60.0;
    legacyWindow.performance.packetLossRatio = 0.0;
    highRefreshDecisions = highRefreshCoordinator.evaluate(configuration, {legacyWindow}, 26000);
    assert(highRefreshDecisions.front().targetFps == 60); // 旧Host或旧DLL始终保留v1原始分辨率/60兼容路径。

    ui::RemoteQualityCoordinator bandwidthCoordinator;
    ui::RemoteQualityWindowMetrics bandwidthLimited = highRefreshWindow;
    bandwidthLimited.windowId = 12;
    bandwidthLimited.receiveFps = 60.0;
    bandwidthLimited.encodedMbps = 30.0;
    bandwidthLimited.performance.packetLossRatio = 0.0;
    bandwidthLimited.performance.availableIncomingBitrateKbps = 25000.0;
    bandwidthCoordinator.evaluate(configuration, {bandwidthLimited}, 1000);
    highRefreshDecisions = bandwidthCoordinator.evaluate(configuration, {bandwidthLimited}, 6000);
    assert(highRefreshDecisions.front().targetFps == 60); // 已知可用带宽低于当前码率余量时优先保住单帧清晰度。
    bandwidthLimited.performance.availableIncomingBitrateKbps = 60000.0;
    bandwidthCoordinator.evaluate(configuration, {bandwidthLimited}, 7000);
    highRefreshDecisions = bandwidthCoordinator.evaluate(configuration, {bandwidthLimited}, 12000);
    assert(highRefreshDecisions.front().targetFps == 60); // wjy: 带宽余量变化也不提升最大窗口的60 FPS上限。
    std::vector<ui::RemoteQualityWindowMetrics> manyWindows;
    manyWindows.reserve(20);
    for (int index = 1; index <= 20; ++index) {
        ui::RemoteQualityWindowMetrics window;
        window.windowId = static_cast<uintptr_t>(index);
        window.active = index == 1;
        window.viewportWidth = index == 1 ? 1920 : 640;
        window.viewportHeight = index == 1 ? 1080 : 360;
        manyWindows.push_back(window);
    }

    ui::RemoteQualityCoordinator countCoordinator;
    auto sixWindowDecisions = countCoordinator.evaluate(
        configuration,
        std::vector<ui::RemoteQualityWindowMetrics>(manyWindows.begin(), manyWindows.begin() + 6),
        30000);
    assert(sixWindowDecisions.front().targetFps == 45);
    for (std::size_t index = 1; index < sixWindowDecisions.size(); ++index) {
        assert(sixWindowDecisions[index].targetFps == 10); // wjy: 6个可见窗口命中多选后台第二档10 FPS。
    }

    manyWindows.front().presenterDropRatio = 0.03;
    auto sixWindowPressureDecisions = countCoordinator.evaluate(
        configuration,
        std::vector<ui::RemoteQualityWindowMetrics>(manyWindows.begin(), manyWindows.begin() + 6),
        30500);
    assert(sixWindowPressureDecisions.front().targetFps == 45);
    for (std::size_t index = 1; index < sixWindowPressureDecisions.size(); ++index) {
        assert(sixWindowPressureDecisions[index].targetFps == 10); // wjy: 单一Presenter噪声不额外降级，仍保持窗口数量对应的10 FPS。
    }
    manyWindows.front().presenterDropRatio = 0.0;

    auto elevenWindowDecisions = countCoordinator.evaluate(
        configuration,
        std::vector<ui::RemoteQualityWindowMetrics>(manyWindows.begin(), manyWindows.begin() + 11),
        31000);
    assert(elevenWindowDecisions.front().targetFps == 45);
    for (std::size_t index = 1; index < elevenWindowDecisions.size(); ++index) {
        assert(elevenWindowDecisions[index].targetFps == 5); // wjy: 11个可见窗口命中第三档5 FPS。
    }

    auto twentyWindowDecisions = countCoordinator.evaluate(configuration, manyWindows, 32000);
    assert(twentyWindowDecisions.front().targetFps == 45);
    for (std::size_t index = 1; index < twentyWindowDecisions.size(); ++index) {
        assert(twentyWindowDecisions[index].targetFps == 3); // wjy: 20个可见窗口命中第四档3 FPS。
    }

    manyWindows.push_back(manyWindows.back());
    manyWindows.back().windowId = 21;
    auto twentyOneWindowDecisions = countCoordinator.evaluate(configuration, manyWindows, 32500);
    assert(twentyOneWindowDecisions.front().targetFps == 45);
    for (std::size_t index = 1; index < twentyOneWindowDecisions.size(); ++index) {
        assert(twentyOneWindowDecisions[index].targetFps == 1); // wjy: 超过20个可见窗口进入多选后台最低1 FPS档。
    }

    manyWindows.front().presenterDropRatio = 0.20;
    manyWindows.front().workerBacklog = true;
    auto severePresenterDecisions = countCoordinator.evaluate(configuration, manyWindows, 33000);
    assert(severePresenterDecisions.front().targetFps == 45);
    for (std::size_t index = 1; index < severePresenterDecisions.size(); ++index) {
        assert(severePresenterDecisions[index].targetFps == 1); // wjy: 双信号持有期内仍保持21窗口本来的1 FPS安全档。
    }
    severePresenterDecisions = countCoordinator.evaluate(configuration, manyWindows, 34100);
    for (std::size_t index = 1; index < severePresenterDecisions.size(); ++index) {
        assert(severePresenterDecisions[index].targetFps == 1); // wjy: 已在最低档时压力策略不能继续降到0或产生无效档位。
    }

    ui::RemoteQualityCoordinator debounceCoordinator;
    ui::RemoteQualityWindowMetrics debounceFirst;
    debounceFirst.windowId = 100;
    debounceFirst.active = true;
    ui::RemoteQualityWindowMetrics debounceSecond;
    debounceSecond.windowId = 101;
    auto debounceDecisions = debounceCoordinator.evaluate(
        configuration, {debounceFirst, debounceSecond}, 1000);
    assert(debounceDecisions[0].requestRemoteProfile);
    debounceFirst.active = false;
    debounceSecond.active = true;
    debounceDecisions = debounceCoordinator.evaluate(
        configuration, {debounceFirst, debounceSecond}, 1100);
    assert(!debounceDecisions[0].requestRemoteProfile);
    assert(!debounceDecisions[1].requestRemoteProfile);
    debounceDecisions = debounceCoordinator.evaluate(
        configuration, {debounceFirst, debounceSecond}, 1500);
    assert(debounceDecisions[0].requestRemoteProfile);
    assert(debounceDecisions[1].requestRemoteProfile); // wjy: 焦点稳定超过350毫秒后才允许远端编码器切换档位。

    // ===end====

    return 0;
}
