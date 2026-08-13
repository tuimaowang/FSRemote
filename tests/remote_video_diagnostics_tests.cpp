#include "stream/RemoteVideoDiagnostics.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
}

} // namespace

int main()
{
    // =====wjy====
    const auto directory = std::filesystem::temp_directory_path() / "fsremote_video_diagnostics_test";
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);

    stream::RemoteVideoDiagnostics diagnostics(
        128,
        64,
        20 * 1000,
        200 * 1000); // wjy: 测试把异常后序窗口缩短到20ms，生产默认仍保留2秒现场并按5秒限频。
    assert(diagnostics.start(directory, 1024, 3));

    stream::RemoteVideoLogEvent lifecycle;
    lifecycle.level = stream::RemoteVideoLogLevel::Info;
    lifecycle.context.runId = 1;
    lifecycle.context.sessionId = 2;
    lifecycle.context.windowId = 3;
    lifecycle.context.viewerGeneration = 4;
    lifecycle.context.frameId = 5;
    lifecycle.event = "surface_register";
    lifecycle.result = "success";
    lifecycle.reason = "test";
    lifecycle.fields.push_back({"frame_age_ms", "12.5"});
    assert(diagnostics.submit(lifecycle));

    stream::RemoteVideoStageSample decodeStage;
    decodeStage.stage = stream::RemoteVideoStage::Decode;
    decodeStage.level = stream::RemoteVideoLogLevel::Trace;
    decodeStage.context = lifecycle.context;
    decodeStage.result = stream::RemoteVideoStageResult::Success;
    decodeStage.reason = stream::RemoteVideoStageReason::NativeFrame;
    decodeStage.durationUs = 321;
    decodeStage.frameAgeUs = 654;
    assert(diagnostics.submitStage(decodeStage)); // wjy: Trace阶段进入环形现场但不进入正常持久化队列。

    stream::RemoteVideoSummary summary;
    summary.scope = stream::RemoteVideoSummaryScope::Session;
    summary.context = lifecycle.context;
    summary.intervalMs = 1000;
    summary.received = 60;
    summary.presented = 58;
    summary.dropped = 2;
    summary.presentedFps = 58.0;
    summary.dropRatio = 2.0 / 60.0;
    summary.frameAgeP95Ms = 18.5;
    summary.averageRenderMs = 1.25;
    summary.workerUtilization = 0.42;
    summary.queueDepth = 1;
    summary.activeSessions = 8;
    summary.cacheHits = 118;
    summary.cacheMisses = 2;
    assert(diagnostics.submitSummary(summary));

    assert(diagnostics.captureAnomaly(
        lifecycle.context, "frame_age_p95", 180.0, 0.3, 9));
    assert(!diagnostics.captureAnomaly(
        lifecycle.context, "frame_age_p95", 181.0, 0.31, 10));
    assert(diagnostics.suppressedAnomalyCount() == 1); // wjy: 同会话同原因在限频窗口内只形成一份前后现场。

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto recovery = lifecycle;
    recovery.level = stream::RemoteVideoLogLevel::Info;
    recovery.context.frameId = 6;
    recovery.event = "recovery";
    recovery.result = "success";
    recovery.reason = "native_frame";
    assert(diagnostics.submit(recovery)); // wjy: 触发后事件进入同一异常dump，验证before/trigger/after完整窗口。

    const std::string payload(300, 'x');
    for (int index = 0; index < 48; ++index) {
        auto rotating = lifecycle;
        rotating.context.frameId = static_cast<std::uint64_t>(100 + index);
        rotating.event = "rotation_probe";
        rotating.fields = {{"payload", payload}};
        assert(diagnostics.submit(std::move(rotating)));
        if (index % 8 == 7) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // wjy: 让LoggerThread持续写入并跨越多个1KB轮转边界。
        }
    }

    for (int attempt = 0; attempt < 100 && diagnostics.anomalyDumpCount() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(diagnostics.anomalyDumpCount() == 1);
    assert(diagnostics.writerThreadId() != 0);
#if defined(_WIN32)
    assert(diagnostics.writerThreadId() != static_cast<std::uint32_t>(::GetCurrentThreadId())); // wjy: 文件写线程与提交测试事件的线程不同，热路径没有同步文件IO。
#endif

    const auto snapshotPath = directory / "snapshot.jsonl";
    assert(diagnostics.snapshot(snapshotPath));
    diagnostics.stop();

    const auto logPath = directory / "video_pipeline.jsonl";
    assert(std::filesystem::exists(logPath));
    assert(std::filesystem::exists(std::filesystem::path(logPath.string() + ".1"))); // wjy: 小文件上限必须触发有界轮转而不是无限增长。
    const std::string snapshotContent = readFile(snapshotPath);
    assert(snapshotContent.find("\"event\":\"stage_decode\"") != std::string::npos);
    assert(snapshotContent.find("\"reason\":\"native_frame\"") != std::string::npos);

    bool foundAnomalyDump = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().rfind("anomaly_", 0) != 0) {
            continue;
        }
        const std::string anomalyContent = readFile(entry.path());
        assert(anomalyContent.find("\"event\":\"anomaly\"") != std::string::npos);
        assert(anomalyContent.find("\"event\":\"recovery\"") != std::string::npos);
        foundAnomalyDump = true;
    }
    assert(foundAnomalyDump);

    std::filesystem::remove_all(directory, cleanupError);
    // ===end====
    return 0;
}
