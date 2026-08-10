#include "system/InputScriptExecutionService.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif // wjy: 禁止windows.h定义min/max宏，避免破坏std::min和numeric_limits::max()模板调用。
#include <windows.h>
#endif

namespace platform {
namespace {

// =====wjy====
constexpr qsizetype kCopyChunkBytes = 256 * 1024;
constexpr qsizetype kMaximumEventsPerTick = 128;
constexpr int kMaximumLoopIntervalMs = 60 * 60 * 1000;
constexpr qint64 kMaximumSharedScriptBytes = 32LL * 1024LL * 1024LL;

bool hashFile(
    const QString& filePath,
    qint64 expectedSize,
    const QString& expectedSha256,
    const std::shared_ptr<std::atomic_bool>& cancelled,
    QString* errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("无法读取键鼠脚本：%1").arg(file.errorString());
        return false;
    }
    if (expectedSize >= 0 && file.size() != expectedSize) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本大小与启动命令不一致。");
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled && cancelled->load(std::memory_order_acquire)) {
            if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本准备已取消。");
            return false;
        }
        const QByteArray chunk = file.read(kCopyChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (errorMessage) *errorMessage = QString::fromUtf8("读取键鼠脚本失败：%1").arg(file.errorString());
            return false;
        }
        hash.addData(chunk); // wjy: 被控端分块计算共享文件哈希，弱网复制不需要一次性占用整份 JSON 内存。
    }
    const QString actualHash = QString::fromLatin1(hash.result().toHex());
    if (actualHash.compare(expectedSha256, Qt::CaseInsensitive) != 0) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本 SHA-256 校验失败。");
        return false;
    }
    return true;
}

bool copySharedScript(
    const QString& sourcePath,
    const QString& temporaryPath,
    qint64 expectedSize,
    const QString& expectedSha256,
    const std::shared_ptr<std::atomic_bool>& cancelled,
    QString* errorMessage)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("无法访问共享键鼠脚本：%1").arg(source.errorString());
        return false;
    }
    if (source.size() != expectedSize) {
        if (errorMessage) *errorMessage = QString::fromUtf8("共享键鼠脚本已变化，请重新选择后执行。");
        return false;
    }

    QFile::remove(temporaryPath); // wjy: 临时路径只由本次 runId 生成，复制前清理同名残留不会影响已验证缓存。
    QFile target(temporaryPath);
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("无法创建键鼠脚本本地缓存：%1").arg(target.errorString());
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copiedBytes = 0;
    while (!source.atEnd()) {
        if (cancelled && cancelled->load(std::memory_order_acquire)) {
            target.close();
            QFile::remove(temporaryPath);
            if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本准备已取消。");
            return false;
        }
        const QByteArray chunk = source.read(kCopyChunkBytes);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            target.close();
            QFile::remove(temporaryPath);
            if (errorMessage) *errorMessage = QString::fromUtf8("读取共享键鼠脚本失败：%1").arg(source.errorString());
            return false;
        }
        if (target.write(chunk) != chunk.size()) {
            target.close();
            QFile::remove(temporaryPath);
            if (errorMessage) *errorMessage = QString::fromUtf8("写入键鼠脚本本地缓存失败：%1").arg(target.errorString());
            return false;
        }
        copiedBytes += chunk.size();
        hash.addData(chunk);
    }
    if (!target.flush()) {
        target.close();
        QFile::remove(temporaryPath);
        if (errorMessage) *errorMessage = QString::fromUtf8("刷新键鼠脚本本地缓存失败：%1").arg(target.errorString());
        return false;
    }
    target.close();

    const QString actualHash = QString::fromLatin1(hash.result().toHex());
    if (copiedBytes != expectedSize || actualHash.compare(expectedSha256, Qt::CaseInsensitive) != 0) {
        QFile::remove(temporaryPath);
        if (errorMessage) *errorMessage = QString::fromUtf8("共享键鼠脚本复制后校验失败。");
        return false;
    }
    return true;
}

int relativePixels(double fraction, int fallback, int dimension)
{
    const int pixels = dimension > 0
        ? static_cast<int>(std::lround(fraction * static_cast<double>(dimension)))
        : fallback;
    return std::clamp(pixels, -200, 200); // wjy: 目标端按自身主屏尺寸还原相对位移，保持原多分辨率回放语义和单事件安全边界。
}

#if defined(Q_OS_WIN)
bool sendWindowsInput(INPUT input)
{
    ::SetLastError(ERROR_SUCCESS);
    return ::SendInput(1, &input, sizeof(input)) == 1;
}

bool moveAbsolute(int x, int y)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = std::clamp(x, 0, 65535);
    input.mi.dy = std::clamp(y, 0, 65535);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    return sendWindowsInput(input);
}

bool moveRelative(int dx, int dy)
{
    if (dx == 0 && dy == 0) return true;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return sendWindowsInput(input);
}

DWORD mouseButtonFlag(int button, bool down)
{
    if (button == 1) return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    if (button == 2) return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    if (button == 4) return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    return 0;
}

bool sendMouseButton(int button, bool down)
{
    const DWORD flag = mouseButtonFlag(button, down);
    if (flag == 0) return false;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    return sendWindowsInput(input);
}

bool sendWheel(int delta)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = static_cast<DWORD>(delta);
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    return sendWindowsInput(input);
}

bool sendKey(int virtualKey, bool down)
{
    if (virtualKey <= 0 || virtualKey > 0xFFFF) return false;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(virtualKey);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return sendWindowsInput(input);
}
#endif
// ===end====

} // namespace

// =====wjy====
QString remoteInputScriptStateName(RemoteInputScriptState state)
{
    switch (state) {
    case RemoteInputScriptState::Idle: return QStringLiteral("idle");
    case RemoteInputScriptState::Preparing: return QStringLiteral("preparing");
    case RemoteInputScriptState::Running: return QStringLiteral("running");
    case RemoteInputScriptState::WaitingLoop: return QStringLiteral("waiting_loop");
    case RemoteInputScriptState::Stopping: return QStringLiteral("stopping");
    case RemoteInputScriptState::Failed: return QStringLiteral("failed");
    case RemoteInputScriptState::Unknown:
    default: return QStringLiteral("unknown");
    }
}

bool remoteInputScriptStateFromName(const QString& name, RemoteInputScriptState* state)
{
    if (!state) return false;
    const QString normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("unknown")) *state = RemoteInputScriptState::Unknown;
    else if (normalized == QStringLiteral("idle")) *state = RemoteInputScriptState::Idle;
    else if (normalized == QStringLiteral("preparing")) *state = RemoteInputScriptState::Preparing;
    else if (normalized == QStringLiteral("running")) *state = RemoteInputScriptState::Running;
    else if (normalized == QStringLiteral("waiting_loop")) *state = RemoteInputScriptState::WaitingLoop;
    else if (normalized == QStringLiteral("stopping")) *state = RemoteInputScriptState::Stopping;
    else if (normalized == QStringLiteral("failed")) *state = RemoteInputScriptState::Failed;
    else return false;
    return true;
}

bool remoteInputScriptStateIsActive(RemoteInputScriptState state)
{
    return state == RemoteInputScriptState::Preparing
        || state == RemoteInputScriptState::Running
        || state == RemoteInputScriptState::WaitingLoop
        || state == RemoteInputScriptState::Stopping; // wjy: 复制、执行、轮间等待和停止清理期间都视为同一目标端任务，禁止再次启动第二份脚本。
}

QString remoteInputScriptSharedDirectory()
{
    return QString::fromUtf8(R"(\\192.168.1.100\广告部工具\远控键鼠脚本)"); // wjy: F9 发布和目标端按需下载固定使用同一共享根目录，命令不得传入任意 UNC 路径。
}

QString remoteInputScriptCacheDirectory()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("data/input-scripts/cache")); // wjy: F9/F10 缓存与主界面脚本 work 目录完全隔离，哈希命名允许同内容长期复用。
}

InputScriptExecutionService& InputScriptExecutionService::instance()
{
    static InputScriptExecutionService service;
    return service;
}

InputScriptExecutionService::InputScriptExecutionService(QObject* parent)
    : QObject(parent)
{
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setSingleShot(true);
    m_playbackTimer->setTimerType(Qt::PreciseTimer); // wjy: 被控端使用本机高精度单次定时器按绝对录制时间调度，控制网络抖动不再参与事件间隔。
    connect(m_playbackTimer, &QTimer::timeout, this, [this] { processDueEvents(); });
}

InputScriptExecutionService::~InputScriptExecutionService()
{
    shutdown();
}

bool InputScriptExecutionService::validStartRequest(
    const RemoteInputScriptStartRequest& request,
    QString* errorMessage)
{
    const QFileInfo fileInfo(request.fileName.trimmed());
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    static const QRegularExpression runIdPattern(QStringLiteral("^[A-Za-z0-9_-]{1,160}$"));
    if (request.runId.trimmed().isEmpty()
        || !runIdPattern.match(request.runId.trimmed()).hasMatch()
        || fileInfo.fileName() != request.fileName.trimmed()
        || !request.fileName.endsWith(QStringLiteral(".fsinput.json"), Qt::CaseInsensitive)
        || !hashPattern.match(request.sha256.trimmed()).hasMatch()
        || request.fileSize <= 0 || request.fileSize > kMaximumSharedScriptBytes
        || request.loopCount < 0 || request.loopCount > 1000000
        || request.loopIntervalMs < 0 || request.loopIntervalMs > kMaximumLoopIntervalMs
        || !std::isfinite(request.speedMultiplier)
        || request.speedMultiplier < 0.10 || request.speedMultiplier > 10.00
        || request.pasteRandomLength < 1 || request.pasteRandomLength > 64
        || request.pasteRandomMode < 0 || request.pasteRandomMode > 2
        || request.pasteRandomSeparator.size() > 64) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本启动参数无效。");
        return false; // wjy: runId和文件名都限制为安全单段文本，临时缓存路径不会被远程命令构造为目录穿越。
    }
    return true;
}

RemoteInputScriptCommandResult InputScriptExecutionService::start(
    const RemoteInputScriptStartRequest& request,
    QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (m_shuttingDown || !validStartRequest(request, errorMessage)) {
        return RemoteInputScriptCommandResult::InvalidRequest;
    }
    const RemoteInputScriptRuntimeInfo current = snapshot();
    if (remoteInputScriptStateIsActive(current.state)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("目标设备已有键鼠脚本正在执行。");
        return RemoteInputScriptCommandResult::AlreadyRunning;
    }

    if (m_prepareThread.joinable()) {
        m_prepareThread.join(); // wjy: 只有上一轮已发布非活动终态后才会进入新 start，同步回收已结束线程不会等待共享目录 IO。
    }
    resetPlaybackData();
    m_request = request;
    m_request.runId = request.runId.trimmed();
    m_request.fileName = request.fileName.trimmed();
    m_request.sha256 = request.sha256.trimmed().toLower();
    m_prepareCancelled = std::make_shared<std::atomic_bool>(false);
    const quint64 generation = ++m_prepareGeneration;
    {
        QMutexLocker locker(&m_statusMutex);
        const quint64 nextRevision = m_status.revision + 1;
        m_status = {};
        m_status.supported = true;
        m_status.state = RemoteInputScriptState::Preparing;
        m_status.runId = m_request.runId;
        m_status.scriptName = m_request.fileName;
        m_status.scriptHash = m_request.sha256;
        m_status.configuredLoops = m_request.loopCount;
        m_status.revision = nextRevision; // wjy: 新一轮启动继续递增目标端版本号，重连窗口可判断快照新旧，不会每次都回到1。
    }
    publishStatusChanged(); // wjy: 先发布 Preparing 再启动共享目录线程，所有监控窗口立即进入目标端权威播放门禁。

    const RemoteInputScriptStartRequest workerRequest = m_request;
    const auto cancelled = m_prepareCancelled;
    try {
        m_prepareThread = std::thread([this, generation, workerRequest, cancelled] {
            PreparationResult result = prepareScript(workerRequest, cancelled);
            QMetaObject::invokeMethod(this,
                [this, generation, workerRequest, result = std::move(result)]() mutable {
                    finishPreparation(generation, workerRequest, std::move(result));
                },
                Qt::QueuedConnection); // wjy: UNC 复制和哈希校验留在后台，脚本状态与 QTimer 只回到目标端 Qt 主线程提交。
        });
    } catch (...) {
        setRuntimeState(RemoteInputScriptState::Failed, QString::fromUtf8("无法创建键鼠脚本准备线程。"));
        if (errorMessage) *errorMessage = QString::fromUtf8("无法创建键鼠脚本准备线程。");
        return RemoteInputScriptCommandResult::Failed;
    }
    return RemoteInputScriptCommandResult::Accepted;
}

InputScriptExecutionService::PreparationResult InputScriptExecutionService::prepareScript(
    const RemoteInputScriptStartRequest& request,
    const std::shared_ptr<std::atomic_bool>& cancelled)
{
    PreparationResult result;
    QDir cacheDirectory(remoteInputScriptCacheDirectory());
    if (!cacheDirectory.mkpath(QStringLiteral("."))) {
        result.errorMessage = QString::fromUtf8("无法创建被控端键鼠脚本缓存目录。");
        return result;
    }

    const QString cachePath = cacheDirectory.filePath(
        request.sha256.toLower() + QStringLiteral(".fsinput.json"));
    QString validationError;
    if (QFileInfo::exists(cachePath)
        && hashFile(cachePath, request.fileSize, request.sha256, cancelled, &validationError)
        && ui::RemoteInputScriptStore::loadFromFile(cachePath, &result.script, &validationError)) {
        result.success = true;
        result.localFilePath = cachePath;
        return result; // wjy: 同一内容已经验证缓存时完全跳过共享目录，后续 F10 只承担一条轻量启动命令。
    }

    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        result.cancelled = true;
        return result;
    }
    QFile::remove(cachePath); // wjy: 同哈希缓存只可能因截断或旧异常写入而失效，重新下载前清掉坏副本避免被再次命中。
    const QString sourcePath = QDir(remoteInputScriptSharedDirectory()).filePath(request.fileName);
    const QString temporaryPath = cacheDirectory.filePath(request.runId + QStringLiteral(".part"));
    if (!copySharedScript(
            sourcePath,
            temporaryPath,
            request.fileSize,
            request.sha256,
            cancelled,
            &result.errorMessage)) {
        result.cancelled = cancelled && cancelled->load(std::memory_order_acquire);
        return result;
    }
    if (!ui::RemoteInputScriptStore::loadFromFile(temporaryPath, &result.script, &result.errorMessage)) {
        QFile::remove(temporaryPath);
        return result; // wjy: 大小和哈希通过后仍执行目标端 JSON 严格校验，畸形共享文件绝不进入 SendInput 调度器。
    }
    if (!QFile::rename(temporaryPath, cachePath)) {
        QFile::remove(temporaryPath);
        result.errorMessage = QString::fromUtf8("无法提交被控端键鼠脚本缓存文件。");
        return result;
    }
    result.success = true;
    result.localFilePath = cachePath;
    return result;
}

void InputScriptExecutionService::finishPreparation(
    quint64 generation,
    const RemoteInputScriptStartRequest& request,
    PreparationResult result)
{
    if (generation != m_prepareGeneration || m_shuttingDown) {
        return; // wjy: 停止或退出已推进代际时丢弃迟到复制结果，旧文件准备不能复活已经取消的运行。
    }
    if (m_prepareThread.joinable()) {
        m_prepareThread.join();
    }
    if (result.cancelled || (m_prepareCancelled && m_prepareCancelled->load(std::memory_order_acquire))) {
        resetPlaybackData();
        setRuntimeState(RemoteInputScriptState::Idle);
        return;
    }
    if (!result.success || result.script.events.isEmpty()) {
        resetPlaybackData();
        setRuntimeState(RemoteInputScriptState::Failed,
            result.errorMessage.trimmed().isEmpty()
                ? QString::fromUtf8("键鼠脚本没有可执行事件。")
                : result.errorMessage.trimmed());
        return;
    }

    m_request = request;
    m_events = std::move(result.script.events);
    m_eventIndex = 0;
    m_completedLoops = 0;
    m_playbackClock.start();
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.eventCount = static_cast<int>(std::min<qsizetype>(m_events.size(), std::numeric_limits<int>::max()));
        m_status.eventIndex = 0;
        m_status.startedAtEpochMs = QDateTime::currentMSecsSinceEpoch();
        m_status.errorMessage.clear();
    }
    setRuntimeState(RemoteInputScriptState::Running); // wjy: 只有目标端本地缓存和 JSON 均验证成功后才发布 Running，控制端不会把命令受理误显示为已经执行。
    processDueEvents();
}

RemoteInputScriptCommandResult InputScriptExecutionService::stop(
    const QString& runId,
    QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    const RemoteInputScriptRuntimeInfo current = snapshot();
    if (!remoteInputScriptStateIsActive(current.state)) {
        return RemoteInputScriptCommandResult::NotRunning;
    }
    if (!runId.trimmed().isEmpty() && runId.trimmed() != current.runId) {
        if (errorMessage) *errorMessage = QString::fromUtf8("运行 ID 已变化，请刷新目标状态后重试。");
        return RemoteInputScriptCommandResult::RunIdMismatch;
    }

    setRuntimeState(RemoteInputScriptState::Stopping);
    if (m_prepareCancelled) {
        m_prepareCancelled->store(true, std::memory_order_release); // wjy: Preparing 阶段只发取消标志，不在命令连接线程等待可能阻塞的共享目录读取。
    }
    if (m_playbackTimer) m_playbackTimer->stop();
    if (!m_events.isEmpty()) {
        releaseHeldInputs();
        resetPlaybackData();
        setRuntimeState(RemoteInputScriptState::Idle);
    }
    return RemoteInputScriptCommandResult::Accepted;
}

void InputScriptExecutionService::processDueEvents()
{
    if (snapshot().state == RemoteInputScriptState::WaitingLoop) {
        m_playbackClock.start();
        setRuntimeState(RemoteInputScriptState::Running); // wjy: 轮间等待结束后重新建立零点，等待时长不参与下一轮事件时间轴。
    }
    if (snapshot().state != RemoteInputScriptState::Running) return;

    qint64 elapsedMs = m_playbackClock.elapsed();
    qsizetype processed = 0;
    while (m_eventIndex < m_events.size()
        && ui::remoteInputScriptPlaybackTimeMs(
                m_events.at(m_eventIndex).elapsedMs,
                m_request.speedMultiplier) <= elapsedMs
        && processed < kMaximumEventsPerTick) {
        if (!injectEvent(m_events.at(m_eventIndex).input)) {
            releaseHeldInputs();
            resetPlaybackData();
            setRuntimeState(RemoteInputScriptState::Failed, QString::fromUtf8("目标端注入键鼠事件失败。"));
            return;
        }
        ++m_eventIndex;
        ++processed;
        elapsedMs = m_playbackClock.elapsed();
    }
    updateRuntimeProgress();

    if (m_eventIndex >= m_events.size()) {
        finishLoop();
        return;
    }
    if (processed >= kMaximumEventsPerTick
        && ui::remoteInputScriptPlaybackTimeMs(
                m_events.at(m_eventIndex).elapsedMs,
                m_request.speedMultiplier) <= elapsedMs) {
        m_playbackTimer->start(0); // wjy: 同时间戳事件拆批让出目标端事件循环，停止命令和状态广播不会被恶意脚本饿死。
        return;
    }
    scheduleNextEvent();
}

void InputScriptExecutionService::scheduleNextEvent()
{
    if (!m_playbackTimer || m_eventIndex >= m_events.size()) return;
    const qint64 eventTimeMs = ui::remoteInputScriptPlaybackTimeMs(
        m_events.at(m_eventIndex).elapsedMs,
        m_request.speedMultiplier);
    const qint64 delayMs = eventTimeMs - m_playbackClock.elapsed();
    m_playbackTimer->start(static_cast<int>(std::clamp<qint64>(
        delayMs, 1, std::numeric_limits<int>::max()))); // wjy: 每次按绝对脚本时间减去本机实耗重新校正，目标 UI 短暂卡顿不会累计成全段漂移。
}

void InputScriptExecutionService::finishLoop()
{
    releaseHeldInputs();
    if (m_completedLoops < std::numeric_limits<int>::max()) ++m_completedLoops;
    updateRuntimeProgress();
    if (!ui::remoteInputScriptShouldRepeat(m_request.loopCount, m_completedLoops)) {
        resetPlaybackData();
        setRuntimeState(RemoteInputScriptState::Idle);
        return;
    }

    m_eventIndex = 0;
    if (m_request.loopIntervalMs > 0) {
        m_playbackClock.invalidate();
        setRuntimeState(RemoteInputScriptState::WaitingLoop);
        m_playbackTimer->start(m_request.loopIntervalMs);
    } else {
        m_playbackClock.restart();
        m_playbackTimer->start(0); // wjy: 零间隔无限循环每轮仍回到事件循环，目标端 F10 停止命令可以及时生效。
    }
}

bool InputScriptExecutionService::injectEvent(const ui::RemoteInputEvent& event)
{
#if defined(Q_OS_WIN)
    if (event.type == ui::RemoteInputEventType::AbsoluteMove
        || event.type == ui::RemoteInputEventType::ButtonDown
        || event.type == ui::RemoteInputEventType::ButtonUp
        || event.type == ui::RemoteInputEventType::Wheel) {
        m_lastNormalizedX = std::clamp(event.normalizedX, 0, 65535);
        m_lastNormalizedY = std::clamp(event.normalizedY, 0, 65535);
    }
    switch (event.type) {
    case ui::RemoteInputEventType::AbsoluteMove:
        return moveAbsolute(m_lastNormalizedX, m_lastNormalizedY);
    case ui::RemoteInputEventType::RelativeMove:
        return moveRelative(
            relativePixels(event.relativeX, event.fallbackDeltaX, ::GetSystemMetrics(SM_CXSCREEN)),
            relativePixels(event.relativeY, event.fallbackDeltaY, ::GetSystemMetrics(SM_CYSCREEN)));
    case ui::RemoteInputEventType::ButtonDown:
        if (!moveAbsolute(m_lastNormalizedX, m_lastNormalizedY)
            || !sendMouseButton(event.button, true)) return false;
        m_heldButtons.insert(event.button);
        return true;
    case ui::RemoteInputEventType::ButtonUp:
        if (!moveAbsolute(m_lastNormalizedX, m_lastNormalizedY)
            || !sendMouseButton(event.button, false)) return false;
        m_heldButtons.remove(event.button);
        return true;
    case ui::RemoteInputEventType::Wheel:
        return moveAbsolute(m_lastNormalizedX, m_lastNormalizedY) && sendWheel(event.wheelDelta);
    case ui::RemoteInputEventType::KeyDown: {
        const bool isControl = event.virtualKey == VK_CONTROL
            || event.virtualKey == VK_LCONTROL
            || event.virtualKey == VK_RCONTROL;
        if (m_request.pasteRandomSuffixEnabled && event.virtualKey == 'V' && m_ctrlDown) {
            QClipboard* clipboard = QGuiApplication::clipboard();
            const QString sourceText = clipboard ? clipboard->text() : QString();
            if (!sourceText.isEmpty() && clipboard) {
                const QString alphabet = m_request.pasteRandomMode == 1
                    ? QStringLiteral("0123456789")
                    : (m_request.pasteRandomMode == 2
                        ? QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
                        : QStringLiteral("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"));
                QString randomText;
                randomText.reserve(m_request.pasteRandomLength);
                for (int index = 0; index < m_request.pasteRandomLength; ++index) {
                    randomText.append(alphabet.at(QRandomGenerator::global()->bounded(alphabet.size())));
                }
                clipboard->setText(sourceText + m_request.pasteRandomSeparator + randomText); // wjy: 随机粘贴直接修改被控端本机剪贴板，主控剪贴板和网络时延不再参与脚本内容。
            }
        }
        if (!sendKey(event.virtualKey, true)) return false;
        m_heldKeys.insert(event.virtualKey);
        if (isControl) m_ctrlDown = true;
        return true;
    }
    case ui::RemoteInputEventType::KeyUp: {
        if (!sendKey(event.virtualKey, false)) return false;
        m_heldKeys.remove(event.virtualKey);
        if (event.virtualKey == VK_CONTROL
            || event.virtualKey == VK_LCONTROL
            || event.virtualKey == VK_RCONTROL) {
            m_ctrlDown = false;
        }
        return true;
    }
    case ui::RemoteInputEventType::CaptureRelease:
        releaseHeldInputs();
        return true;
    }
#else
    Q_UNUSED(event)
#endif
    return false;
}

void InputScriptExecutionService::releaseHeldInputs()
{
#if defined(Q_OS_WIN)
    const QSet<int> heldKeys = m_heldKeys;
    const QSet<int> heldButtons = m_heldButtons;
    m_heldKeys.clear();
    m_heldButtons.clear();
    m_ctrlDown = false;
    for (const int virtualKey : heldKeys) sendKey(virtualKey, false);
    moveAbsolute(m_lastNormalizedX, m_lastNormalizedY);
    for (const int button : heldButtons) sendMouseButton(button, false); // wjy: 停止、自然结束和失败都由被控端本地补齐抬起，主控断线不会在目标桌面留下按住状态。
#endif
}

void InputScriptExecutionService::resetPlaybackData()
{
    if (m_playbackTimer) m_playbackTimer->stop();
    m_playbackClock.invalidate();
    m_events.clear();
    m_eventIndex = 0;
    m_completedLoops = 0;
    m_heldKeys.clear();
    m_heldButtons.clear();
    m_ctrlDown = false;
}

void InputScriptExecutionService::setRuntimeState(
    RemoteInputScriptState state,
    const QString& errorMessage)
{
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.state = state;
        m_status.errorMessage = errorMessage.trimmed();
        if (state == RemoteInputScriptState::Idle) {
            m_status.runId.clear();
            m_status.scriptName.clear();
            m_status.scriptHash.clear();
            m_status.completedLoops = 0;
            m_status.configuredLoops = 1;
            m_status.eventIndex = 0;
            m_status.eventCount = 0;
            m_status.startedAtEpochMs = 0;
            m_status.errorMessage.clear();
        }
        ++m_status.revision;
    }
    publishStatusChanged();
}

void InputScriptExecutionService::updateRuntimeProgress()
{
    QMutexLocker locker(&m_statusMutex);
    m_status.completedLoops = m_completedLoops;
    m_status.eventIndex = static_cast<int>(std::min<qsizetype>(m_eventIndex, std::numeric_limits<int>::max()));
}

RemoteInputScriptRuntimeInfo InputScriptExecutionService::snapshot() const
{
    QMutexLocker locker(&m_statusMutex);
    return m_status;
}

void InputScriptExecutionService::setStatusChangedCallback(std::function<void()> callback)
{
    m_statusChangedCallback = std::move(callback);
}

void InputScriptExecutionService::publishStatusChanged()
{
    if (m_statusChangedCallback) m_statusChangedCallback(); // wjy: 状态变化只唤醒现有实时广播服务，执行器不依赖 DeviceGrid 或任何远控窗口对象。
}

void InputScriptExecutionService::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    ++m_prepareGeneration;
    if (m_prepareCancelled) m_prepareCancelled->store(true, std::memory_order_release);
    if (m_playbackTimer) m_playbackTimer->stop();
    releaseHeldInputs();
    resetPlaybackData();
    if (m_prepareThread.joinable()) m_prepareThread.join(); // wjy: 目标程序退出前汇合共享目录读取线程，防止后台回调访问已经析构的 Qt 对象。
    setRuntimeState(RemoteInputScriptState::Idle);
    m_statusChangedCallback = {};
}
// ===end====

} // namespace platform
