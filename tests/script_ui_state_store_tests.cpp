#include "ui/ScriptUiStateStore.h"

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    bool passed = true;
    ui::ScriptUiStateStore store;

    // =====wjy====
    ui::ScriptUiState first;
    first.outputRunning = true;
    first.remoteRunId = QStringLiteral("run-a");
    first.editorText = QStringLiteral("device-a");
    first.cancelRequested = std::make_shared<std::atomic_bool>(false);
    store.setState(QStringLiteral(" 192.168.1.10 "), first); // wjy: 写入带空格 IP，验证仓储统一规范化键。

    ui::ScriptUiState second;
    second.remoteRunId = QStringLiteral("run-b");
    second.editorText = QStringLiteral("device-b");
    second.cancelRequested = std::make_shared<std::atomic_bool>(false);
    store.setState(QStringLiteral("192.168.1.11"), second);

    passed &= expect(store.state(QStringLiteral("192.168.1.10")).remoteRunId == QStringLiteral("run-a"),
        "normalized device key did not preserve the first run identity");
    passed &= expect(store.state(QStringLiteral("192.168.1.11")).editorText == QStringLiteral("device-b"),
        "per-device editor state was not isolated");

    store.requestCancelAll();
    passed &= expect(first.cancelRequested->load() && second.cancelRequested->load(),
        "application shutdown did not cancel every device script");

    store.removeState(QStringLiteral("192.168.1.10"));
    passed &= expect(store.state(QStringLiteral("192.168.1.10")).remoteRunId.isEmpty(),
        "removed device retained stale script state");

    // =====wjy====
    ui::ScriptUiState transition;
    transition.outputVisible = true;
    transition.outputRunning = true; // wjy: 执行中的脚本同时显示输出面板和运行状态。
    transition.localLaunchInProgress = true;
    transition.remoteRunId = QStringLiteral("run-transition");
    transition.outputText = QStringLiteral("line-1\nline-2"); // wjy: 输出文本保持可复制的原始顺序，不由状态仓储截断或改写。
    transition.outputFilePath = QStringLiteral("C:/scripts/demo.ps1");
    transition.cancelRequested = std::make_shared<std::atomic_bool>(false);
    transition.editorVisible = true;
    transition.editorModified = true;
    transition.editorRequestId = QStringLiteral("editor-request-1"); // wjy: 编辑器异步请求身份与脚本运行身份独立保存，迟到回调不能覆盖新请求。
    transition.editorText = QStringLiteral("Write-Host demo");
    store.setState(QStringLiteral("192.168.1.12"), transition);

    ui::ScriptUiState executing = store.state(QStringLiteral("192.168.1.12"));
    passed &= expect(executing.outputRunning && executing.localLaunchInProgress,
        "execute transition did not retain running state");
    passed &= expect(executing.outputText == QStringLiteral("line-1\nline-2"),
        "copyable script output was not preserved");
    passed &= expect(executing.editorVisible && executing.editorModified,
        "editor visibility or dirty state was not preserved");

    executing.cancelRequested->store(true); // wjy: 停止动作先发出取消标志，后台会话随后依据同一共享标志退出。
    executing.outputRunning = false;
    executing.localLaunchInProgress = false;
    executing.remoteStatusConfirmed = true; // wjy: 目标端确认停止后仍保留本次 runId，供迟到回调做身份校验。
    store.setState(QStringLiteral("192.168.1.12"), executing);
    const ui::ScriptUiState stopped = store.state(QStringLiteral("192.168.1.12"));
    passed &= expect(!stopped.outputRunning && stopped.cancelRequested->load(),
        "stop transition did not retain cancellation state");
    passed &= expect(stopped.remoteRunId == QStringLiteral("run-transition"),
        "stop transition lost run identity before late callbacks were rejected");

    ui::ScriptUiState recovered = stopped;
    recovered.outputFailed = true;
    recovered.outputTitle = QStringLiteral("脚本失败，可恢复");
    recovered.outputVisible = true;
    recovered.editorRequestId = QStringLiteral("editor-request-2"); // wjy: 恢复编辑器时生成新请求身份，旧读取结果必须被忽略。
    store.setState(QStringLiteral("192.168.1.12"), recovered);
    const ui::ScriptUiState restored = store.state(QStringLiteral("192.168.1.12"));
    passed &= expect(restored.outputFailed && restored.outputVisible,
        "recoverable script output state was not retained");
    passed &= expect(restored.editorRequestId == QStringLiteral("editor-request-2"),
        "editor recovery did not replace the request identity");
    // ===end====
    // ===end====

    return passed ? 0 : 1;
}
