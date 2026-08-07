#include "stream/RemoteVideoPolicy.h"

#include <cassert>

int main()
{
    const stream::RemoteVideoProfile focused{1920, 1080, 60, 100, true, 48000, stream::RemoteResolutionTier::P1080}; // wjy: 测试焦点配置同步包含统一分辨率档位字段。
    std::vector<stream::RemoteVideoWindowState> states = {
        {1, true, true, false, true, true},
        {2, true, true, false, true, false},
        {3, true, false, true, true, false},
        {4, false, true, false, true, false},
    };

    const auto healthy = stream::RemoteVideoPolicy::resolve(
        states,
        focused,
        {});
    assert(healthy.size() == states.size());
    assert(healthy[0].role == stream::RemoteVideoWindowRole::Focused);
    assert(healthy[0].profile.resolution == stream::RemoteResolutionTier::P1080);
    assert(healthy[0].profile.targetFps == 60);
    assert(healthy[1].role == stream::RemoteVideoWindowRole::VisibleBackground);
    assert(healthy[1].profile.width == 1280);
    assert(healthy[1].profile.height == 720);
    assert(healthy[1].profile.targetFps == 30);
    assert(healthy[1].profile.resolution == stream::RemoteResolutionTier::P720);
    assert(healthy[2].role == stream::RemoteVideoWindowRole::Minimized);
    assert(healthy[2].profile.targetFps == 1);
    assert(healthy[3].role == stream::RemoteVideoWindowRole::Hidden);

    stream::RemoteVideoPressureSignals pressure;
    pressure.workerBacklog = true;
    pressure.presentDrops = true;
    const auto degraded = stream::RemoteVideoPolicy::resolve(states, focused, pressure);
    assert(degraded[0].profile.targetFps == 60);
    assert(degraded[1].emergency == stream::RemoteVideoEmergencyTier::Fps15);
    assert(degraded[1].profile.targetFps == 30); // wjy: 压力等级仍可诊断，但不再通过旧FPS梯度改写正常后台配置。

    pressure.frameAgeHigh = true;
    pressure.syncBusy = true;
    const auto severe = stream::RemoteVideoPolicy::resolve(states, focused, pressure);
    assert(severe[1].profile.targetFps == 30); // wjy: 严重压力不再恢复15/10/5/3/1的后台降帧策略。

    stream::RemoteVideoPressureController controller;
    assert(controller.update(pressure, 1000) == stream::RemoteVideoEmergencyTier::None);
    assert(controller.update(pressure, 2100) == stream::RemoteVideoEmergencyTier::Fps5);
    assert(controller.update({}, 3000) == stream::RemoteVideoEmergencyTier::Fps5);
    assert(controller.update({}, 6100) == stream::RemoteVideoEmergencyTier::Fps10);
    stream::RemoteVideoPressureSignals deviceFailure;
    deviceFailure.deviceFailure = true;
    assert(controller.update(deviceFailure, 6200) == stream::RemoteVideoEmergencyTier::Fps1);
    return 0;
}
