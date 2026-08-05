#include "stream/RemoteQualityPolicy.h"

#include <cassert>

int main()
{
    // =====wjy====
    const stream::RemoteQualityConfiguration defaults =
        stream::normalizedRemoteQualityConfiguration({});
    assert(defaults.targetFps == 60); // wjy: 可见窗口默认优先保证60 FPS。
    assert(defaults.minimumVisibleFps == 30);
    assert(defaults.severePressureMinimumFps == 30); // wjy: 15 FPS退出可见策略，仅作为统一最小化后台档。
    assert(defaults.minimizedFps == 1); // wjy: 最小化立即降到1 FPS但不使用0暂停流，恢复时无需重建连接。
    assert(defaults.degradationHoldMs == 6000);
    assert(defaults.recoveryHoldMs == 3000); // wjy: 可见降档证据更长，健康恢复更快，冷却逻辑单独负责防振荡。
    assert(defaults.minimumVisibleResolution == stream::RemoteResolutionTier::P720);
    assert(defaults.minimizedResolution == stream::RemoteResolutionTier::P360); // wjy: 默认远端请求与本地最小化640x360安全档一致。

    stream::RemoteQualityConfiguration malformed;
    malformed.defaultMode = stream::RemoteQualityMode::FollowGlobal;
    malformed.targetFps = 200;
    malformed.minimumVisibleFps = 120;
    malformed.severePressureMinimumFps = 80;
    malformed.minimizedFps = 0;
    malformed.minimumVisibleResolution = static_cast<stream::RemoteResolutionTier>(99);
    malformed.minimizedResolution = static_cast<stream::RemoteResolutionTier>(-1);
    malformed.degradationHoldMs = 0;
    malformed.recoveryHoldMs = 100;
    malformed.aggregateReceiveBudgetMbps = 5000;

    const stream::RemoteQualityConfiguration normalized =
        stream::normalizedRemoteQualityConfiguration(malformed);
    assert(normalized.defaultMode == stream::RemoteQualityMode::Automatic); // wjy: 全局FollowGlobal无效，必须回退自动。
    assert(normalized.targetFps == 60);
    assert(normalized.minimumVisibleFps == 60);
    assert(normalized.severePressureMinimumFps == 60);
    assert(normalized.minimizedFps == 1); // wjy: 异常0 FPS被夹到1，保证协议语义始终是不断流。
    assert(normalized.minimumVisibleResolution == stream::RemoteResolutionTier::P720);
    assert(normalized.minimizedResolution == stream::RemoteResolutionTier::P360);
    assert(normalized.degradationHoldMs == 500);
    assert(normalized.recoveryHoldMs == 500); // wjy: 恢复时间独立夹紧到安全下限，可短于普通降档证据窗。
    assert(normalized.aggregateReceiveBudgetMbps == 2000);

    for (std::size_t index = 1; index < stream::kRemoteFpsTiers.size(); ++index) {
        assert(stream::kRemoteFpsTiers[index - 1] > stream::kRemoteFpsTiers[index]); // wjy: 固定FPS档位必须严格从高到低，协调器才能安全逐级降档。
    }
    assert(stream::kRemoteFpsTiers.size() == 5);
    assert(stream::kRemoteFpsTiers[0] == 60);
    assert(stream::kRemoteFpsTiers[1] == 45);
    assert(stream::kRemoteFpsTiers[2] == 30);
    assert(stream::kRemoteFpsTiers[3] == 24);
    assert(stream::kRemoteFpsTiers[4] == 15); // wjy: 自动只使用前三档，软件回退和最小化分别使用24与15安全档。
    for (std::size_t index = 1; index < stream::kRemoteResolutionTiers.size(); ++index) {
        assert(static_cast<int>(stream::kRemoteResolutionTiers[index - 1])
            < static_cast<int>(stream::kRemoteResolutionTiers[index])); // wjy: 分辨率枚举顺序必须严格从高到低质量。
    }
    // ===end====

    return 0;
}
