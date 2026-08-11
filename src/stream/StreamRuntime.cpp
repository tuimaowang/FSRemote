#include "stream/StreamRuntime.h"

#include "system/PortableOpenSshManager.h"

#include <QCoreApplication>
#include <QLibrary>

#include <cstring>

namespace {

// =====wjy====
uint32_t copyIdentityBytes(const QByteArray& source, uint8_t* output, uint32_t outputCapacity)
{
    const uint32_t required = static_cast<uint32_t>(source.size());
    if (!output || outputCapacity < required) return required; // wjy: DLL 可先查询长度或使用固定上限，容量不足时不写半截密钥/签名。
    if (required > 0) std::memcpy(output, source.constData(), required);
    return required;
}

uint32_t FSREMOTE_STREAM_CALL readSessionPublicKey(void*, uint8_t* output, uint32_t outputCapacity)
{
    QString error;
    const QByteArray key = platform::PortableOpenSshManager::instance().clientPublicKey(&error).toUtf8();
    return error.isEmpty() ? copyIdentityBytes(key, output, outputCapacity) : 0;
}

uint32_t FSREMOTE_STREAM_CALL signSessionChallenge(
    void*,
    const uint8_t* challenge,
    uint32_t challengeSize,
    uint8_t* output,
    uint32_t outputCapacity)
{
    if (!challenge || challengeSize == 0) return 0;
    QString error;
    const QByteArray signature = platform::PortableOpenSshManager::instance().signSessionChallenge(
        QByteArray(reinterpret_cast<const char*>(challenge), static_cast<int>(challengeSize)), &error);
    return error.isEmpty() ? copyIdentityBytes(signature, output, outputCapacity) : 0;
}

int FSREMOTE_STREAM_CALL isSessionPublicKeyAuthorized(void*, const uint8_t* publicKey, uint32_t publicKeySize)
{
    if (!publicKey || publicKeySize == 0) return 0;
    QString error;
    return platform::PortableOpenSshManager::instance().isSessionPublicKeyAuthorized(
        QString::fromUtf8(reinterpret_cast<const char*>(publicKey), static_cast<int>(publicKeySize)), &error) ? 1 : 0;
}

int FSREMOTE_STREAM_CALL verifySessionChallenge(
    void*,
    const uint8_t* publicKey,
    uint32_t publicKeySize,
    const uint8_t* challenge,
    uint32_t challengeSize,
    const uint8_t* signature,
    uint32_t signatureSize)
{
    if (!publicKey || !challenge || !signature || publicKeySize == 0 || challengeSize == 0 || signatureSize == 0) return 0;
    QString error;
    return platform::PortableOpenSshManager::instance().verifySessionChallenge(
        QString::fromUtf8(reinterpret_cast<const char*>(publicKey), static_cast<int>(publicKeySize)),
        QByteArray(reinterpret_cast<const char*>(challenge), static_cast<int>(challengeSize)),
        QByteArray(reinterpret_cast<const char*>(signature), static_cast<int>(signatureSize)),
        &error) ? 1 : 0;
}
// ===end====

} // namespace

namespace stream {

StreamRuntime& StreamRuntime::instance()
{
    static StreamRuntime runtime;
    return runtime;
}

StreamRuntime::StreamRuntime()
{
    auto* library = new QLibrary(QCoreApplication::applicationDirPath() + QStringLiteral("/fsremote_stream.dll"));
    if (!library->load()) {
        m_error = library->errorString();
        delete library;
        return;
    }

    m_startHost = reinterpret_cast<StartHostFn>(library->resolve("fsremote_stream_start_host"));
    m_setIdentityCallbacks = reinterpret_cast<SetIdentityCallbacksFn>(library->resolve("fsremote_stream_set_identity_callbacks"));
    m_startHostWithConfig = reinterpret_cast<StartHostWithConfigFn>(library->resolve("fsremote_stream_start_host_with_config")); // wjy: 新 DLL 优先接收容量和握手配置，旧 DLL 仍可走原始入口。
    m_startViewer = reinterpret_cast<StartViewerFn>(library->resolve("fsremote_stream_start_viewer"));
    m_startViewerWithStatus = reinterpret_cast<StartViewerWithStatusFn>(library->resolve("fsremote_stream_start_viewer_with_status"));
    m_startViewerWithTexture = reinterpret_cast<StartViewerWithTextureFn>(library->resolve("fsremote_stream_start_viewer_with_texture"));
    m_startViewerWithTextureRole = reinterpret_cast<StartViewerWithTextureRoleFn>(
        library->resolve("fsremote_stream_start_viewer_with_texture_role")); // wjy: 只读监控由认证角色保证，不能仅依赖Qt层不发送键鼠。
    m_stop = reinterpret_cast<StopFn>(library->resolve("fsremote_stream_stop"));
    m_sendInput = reinterpret_cast<SendInputFn>(library->resolve("fsremote_stream_send_input"));
    m_setViewerQuality = reinterpret_cast<SetViewerQualityFn>(library->resolve("fsremote_stream_set_viewer_quality")); // wjy: 可选导出支持无重连画质更新，旧DLL继续使用原始流。
    m_setViewerAudioEnabled = reinterpret_cast<SetViewerAudioEnabledFn>(library->resolve("fsremote_stream_set_viewer_audio_enabled")); // wjy: 新DLL支持焦点切换时在线启停音频，旧DLL不影响视频兼容路径。
    m_getViewerQualityStatus = reinterpret_cast<GetViewerQualityStatusFn>(library->resolve("fsremote_stream_get_viewer_quality_status"));
    m_getViewerPerformanceStats = reinterpret_cast<GetViewerPerformanceStatsFn>(library->resolve("fsremote_stream_get_viewer_performance_stats")); // wjy: 性能统计导出保持可选，避免新 UI 强制依赖旧 DLL 不具备的能力。
    m_isBusy = reinterpret_cast<IsBusyFn>(library->resolve("fsremote_stream_is_busy"));
    // =====wjy====
    m_activeSessionCount = reinterpret_cast<ActiveSessionCountFn>(library->resolve("fsremote_stream_active_session_count")); // wjy: 新 DLL 导出真实会话数；旧 DLL 缺失时状态服务回退 busy 布尔。
    m_activeControllerNames = reinterpret_cast<ActiveControllerNamesFn>(library->resolve("fsremote_stream_active_controller_names"));
    m_activeControllerDetails = reinterpret_cast<ActiveControllerDetailsFn>(library->resolve("fsremote_stream_active_controller_details")); // wjy: 目标端提示层读取控制端设备名与来源 IP；旧 DLL 缺失时安全回退为空。
    // ===end====
    m_lastError = reinterpret_cast<LastErrorFn>(library->resolve("fsremote_stream_last_error"));
    m_loaded = m_startHost && m_startViewer && m_stop && m_lastError;
    if (!m_loaded) {
        m_error = QStringLiteral("fsremote_stream.dll exports are incomplete");
    }
    // =====wjy====
    if (m_setIdentityCallbacks) {
        FsRemoteIdentityCallbacks callbacks = {};
        callbacks.struct_size = sizeof(callbacks);
        callbacks.version = 1;
        callbacks.read_public_key = &readSessionPublicKey;
        callbacks.sign_challenge = &signSessionChallenge;
        callbacks.is_public_key_authorized = &isSessionPublicKeyAuthorized;
        callbacks.verify_challenge = &verifySessionChallenge;
        m_setIdentityCallbacks(&callbacks); // wjy: DLL 立即复制函数表，后续工作线程通过 Qt 现有 OpenSSH 管理器完成签名和验签。
    }
    // ===end====
}

bool StreamRuntime::isLoaded() const
{
    return m_loaded;
}

QString StreamRuntime::lastError() const
{
    if (m_lastError) {
        const char* error = m_lastError();
        if (error && *error) {
            return QString::fromLocal8Bit(error);
        }
    }
    return m_error;
}

FsRemoteStreamHandle StreamRuntime::startHost(uint16_t port)
{
    return m_startHost ? m_startHost(port) : nullptr;
}

// =====wjy====
FsRemoteStreamHandle StreamRuntime::startHost(uint16_t port, const FsRemoteHostConfig& config)
{
    if (m_startHostWithConfig) {
        return m_startHostWithConfig(port, &config); // wjy: 配置结构在同步 DLL 调用期间保持有效，DLL 会立即复制并规范化字段。
    }
    return startHost(port); // wjy: 兼容尚未导出配置入口的旧版 fsremote_stream.dll，保持单会话行为。
}
// ===end====

FsRemoteStreamHandle StreamRuntime::startViewer(
    const QString& hostIp,
    uint16_t port,
    FsRemoteFrameCallback callback,
    void* user)
{
    if (!m_startViewer) {
        return nullptr;
    }
    const QByteArray ip = hostIp.toUtf8();
    return m_startViewer(ip.constData(), port, callback, user);
}

FsRemoteStreamHandle StreamRuntime::startViewer(
    const QString& hostIp,
    uint16_t port,
    FsRemoteFrameCallback frameCallback,
    FsRemoteStatusCallback statusCallback,
    void* user)
{
    const QByteArray ip = hostIp.toUtf8();
    if (m_startViewerWithStatus) {
        return m_startViewerWithStatus(ip.constData(), port, frameCallback, statusCallback, user);
    }
    if (statusCallback) {
        statusCallback(user, 0, "DLL status callback export is unavailable");
    }
    return m_startViewer ? m_startViewer(ip.constData(), port, frameCallback, user) : nullptr;
}

FsRemoteStreamHandle StreamRuntime::startViewer(
    const QString& hostIp,
    uint16_t port,
    FsRemoteFrameCallback frameCallback,
    FsRemoteTextureFrameCallback textureCallback,
    FsRemoteStatusCallback statusCallback,
    void* user,
    bool monitorReadOnly)
{
    const QByteArray ip = hostIp.toUtf8();
    // =====wjy====
    if (m_startViewerWithTextureRole) {
        return m_startViewerWithTextureRole(
            ip.constData(),
            port,
            frameCallback,
            textureCallback,
            statusCallback,
            user,
            monitorReadOnly ? FSREMOTE_VIEWER_ROLE_MONITOR : FSREMOTE_VIEWER_ROLE_CONTROL); // wjy: 普通和监控会话从认证握手开始隔离权限与人数统计。
    }
    if (monitorReadOnly) {
        if (statusCallback) {
            statusCallback(user, FSREMOTE_STATUS_ERROR, "Read-only monitor viewer is unavailable in this DLL"); // wjy: 旧DLL只能请求控制角色，明确失败可避免监控窗口偷偷占用远控名额。
        }
        return nullptr;
    }
    // ===end====
    if (m_startViewerWithTexture) {
        return m_startViewerWithTexture(ip.constData(), port, frameCallback, textureCallback, statusCallback, user);
    }
    return startViewer(hostIp, port, frameCallback, statusCallback, user);
}

void StreamRuntime::stop(FsRemoteStreamHandle handle)
{
    if (m_stop && handle) {
        m_stop(handle);
    }
}

bool StreamRuntime::sendInput(FsRemoteStreamHandle handle, const QByteArray& message)
{
    return m_sendInput && handle && !message.isEmpty() && m_sendInput(handle, message.constData()) != 0;
}

// =====wjy====
bool StreamRuntime::setViewerQuality(FsRemoteStreamHandle handle, const FsRemoteViewerQualityConfig& config)
{
    return m_setViewerQuality && handle && m_setViewerQuality(handle, &config) != 0; // wjy: DLL同步复制配置，调用返回后栈上结构即可释放。
}

bool StreamRuntime::setViewerAudioEnabled(FsRemoteStreamHandle handle, bool enabled)
{
    return m_setViewerAudioEnabled && handle
        && m_setViewerAudioEnabled(handle, enabled ? 1 : 0) != 0; // wjy: 同步保存最新意图；准入尚未完成时由Viewer内部延后应用。
}

bool StreamRuntime::viewerQualityStatus(FsRemoteStreamHandle handle, FsRemoteViewerQualityStatus* status) const
{
    if (!m_getViewerQualityStatus || !handle || !status) {
        return false;
    }
    return m_getViewerQualityStatus(handle, status) != 0; // wjy: 缺失导出或尚无确认都返回false，UI保持不断流并显示不支持/等待状态。
}

bool StreamRuntime::viewerPerformanceStats(FsRemoteStreamHandle handle, FsRemoteViewerPerformanceStats* stats) const
{
    if (!m_getViewerPerformanceStats || !handle || !stats) {
        return false;
    }
    return m_getViewerPerformanceStats(handle, stats) != 0; // wjy: DLL 在互斥区内复制 POD 快照，UI 只做相邻采样差分，不阻塞 WebRTC 线程。
}
// ===end====

bool StreamRuntime::isBusy(FsRemoteStreamHandle handle) const
{
    return m_isBusy && handle && m_isBusy(handle) != 0;
}

// =====wjy====
uint32_t StreamRuntime::activeSessionCount(FsRemoteStreamHandle handle) const
{
    if (m_activeSessionCount && handle) {
        return m_activeSessionCount(handle); // wjy: 优先使用 DLL 真实会话计数。
    }
    return isBusy(handle) ? 1u : 0u; // wjy: 旧 DLL 没有计数导出时，busy 视为至少 1 路，保持徽标可见。
}

QString StreamRuntime::activeControllerNames(FsRemoteStreamHandle handle) const
{
    if (!m_activeControllerNames || !handle) {
        return {};
    }
    const uint32_t needed = m_activeControllerNames(handle, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    QByteArray buffer(static_cast<int>(needed), Qt::Uninitialized);
    const uint32_t written = m_activeControllerNames(handle, buffer.data(), needed);
    if (written == 0) {
        return {};
    }
    return QString::fromUtf8(buffer.constData(), static_cast<int>(written));
}

QString StreamRuntime::activeControllerDetails(FsRemoteStreamHandle handle) const
{
    if (!m_activeControllerDetails || !handle) {
        return {};
    }
    const uint32_t needed = m_activeControllerDetails(handle, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    QByteArray buffer(static_cast<int>(needed), Qt::Uninitialized);
    const uint32_t written = m_activeControllerDetails(handle, buffer.data(), needed);
    if (written == 0) {
        return {};
    }
    return QString::fromUtf8(buffer.constData(), static_cast<int>(written));
}
// ===end====

} // namespace stream
