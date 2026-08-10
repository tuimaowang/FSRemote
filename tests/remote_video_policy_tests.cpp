#include "stream/RemoteVideoPolicy.h"

#include <cassert>

int main()
{
    // =====wjy====
    assert(stream::kRemoteVideo1080p60Preset.resolution == stream::RemoteResolutionTier::P1080
        && stream::kRemoteVideo1080p60Preset.targetFps == 60
        && stream::kRemoteVideo1080p60Preset.maxBitrateKbps == 48000); // wjy: 验证1080p/60 FPS命名档案的三个编码参数始终成套。
    assert(stream::kRemoteVideo1080p30Preset.resolution == stream::RemoteResolutionTier::P1080
        && stream::kRemoteVideo1080p30Preset.targetFps == 30
        && stream::kRemoteVideo1080p30Preset.maxBitrateKbps == 48000); // wjy: 验证1080p/30 FPS档只降帧率而不改变清晰度预算。
    assert(stream::kRemoteVideo720p60Preset.resolution == stream::RemoteResolutionTier::P720
        && stream::kRemoteVideo720p60Preset.targetFps == 60
        && stream::kRemoteVideo720p60Preset.maxBitrateKbps == 24000); // wjy: 验证720p/60 FPS流畅档的固定映射。
    assert(stream::kRemoteVideo720p30Preset.resolution == stream::RemoteResolutionTier::P720
        && stream::kRemoteVideo720p30Preset.targetFps == 30
        && stream::kRemoteVideo720p30Preset.maxBitrateKbps == 24000); // wjy: 验证720p/30 FPS平衡档的固定映射。
    assert(stream::kRemoteVideo540p30Preset.resolution == stream::RemoteResolutionTier::P540
        && stream::kRemoteVideo540p30Preset.targetFps == 30
        && stream::kRemoteVideo540p30Preset.maxBitrateKbps == 14000); // wjy: 验证当前焦点选用的540p/30 FPS档已降到14 Mbps上限。
    assert(stream::kRemoteVideo540p25Preset.resolution == stream::RemoteResolutionTier::P540
        && stream::kRemoteVideo540p25Preset.targetFps == 25
        && stream::kRemoteVideo540p25Preset.maxBitrateKbps == 14000); // wjy: 验证当前可见后台选用的540p/25 FPS档。
    assert(stream::kRemoteVideo360p30Preset.resolution == stream::RemoteResolutionTier::P360
        && stream::kRemoteVideo360p30Preset.targetFps == 30
        && stream::kRemoteVideo360p30Preset.maxBitrateKbps == 7000); // wjy: 验证360p/30 FPS交互档保留7 Mbps上限。
    assert(stream::kRemoteVideo360p1Preset.resolution == stream::RemoteResolutionTier::P360
        && stream::kRemoteVideo360p1Preset.targetFps == 1
        && stream::kRemoteVideo360p1Preset.maxBitrateKbps == 7000); // wjy: 验证360p/1 FPS保活档的尺寸、帧率和码率集合。

    const stream::RemoteVideoProfile focused = stream::kFocusedRemoteVideoProfile; // wjy: 策略测试直接使用生产焦点别名，避免测试里再手工拼一套过时参数。
    // ===end====
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
    assert(healthy[0].profile.resolution == stream::RemoteResolutionTier::P540);
    assert(healthy[0].profile.targetFps == 30);
    assert(healthy[0].profile.maxBitrateKbps == 14000); // wjy: 焦点角色必须完整套用540p/30 FPS/14 Mbps档案。
    assert(healthy[0].profile.priority == 100); // wjy: 编码档不包含角色语义，焦点别名单独补充最高优先级。
    assert(healthy[1].role == stream::RemoteVideoWindowRole::VisibleBackground);
    assert(healthy[1].profile.resolution == stream::RemoteResolutionTier::P540);
    assert(healthy[1].profile.targetFps == 25);
    assert(healthy[1].profile.maxBitrateKbps == 14000); // wjy: 可见后台与焦点共享540p清晰度预算，只降到25 FPS。
    assert(healthy[1].profile.priority == 40); // wjy: 后台角色使用配置的中等优先级，不再被协调器强制改成10。
    assert(healthy[2].role == stream::RemoteVideoWindowRole::Minimized);
    assert(healthy[2].profile.targetFps == 1);
    assert(healthy[2].profile.priority == 5); // wjy: 最小化角色保留最低优先级和不断流语义。
    assert(healthy[3].role == stream::RemoteVideoWindowRole::Hidden);

    stream::RemoteVideoPressureSignals pressure;
    pressure.workerBacklog = true;
    pressure.presentDrops = true;
    const auto degraded = stream::RemoteVideoPolicy::resolve(states, focused, pressure);
    assert(degraded[0].profile.targetFps == 30);
    assert(degraded[1].emergency == stream::RemoteVideoEmergencyTier::Fps15);
    assert(degraded[1].profile.targetFps == 25); // wjy: 压力等级仍可诊断，但不改写已选的540p/25 FPS后台档。

    pressure.frameAgeHigh = true;
    pressure.syncBusy = true;
    const auto severe = stream::RemoteVideoPolicy::resolve(states, focused, pressure);
    assert(severe[1].profile.targetFps == 25); // wjy: 严重压力不再恢复15/10/5/3/1的后台降帧策略。

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
