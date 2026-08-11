#pragma once

#include "FsRemoteStreamApi.h"

#include <QByteArray>
#include <QString>

namespace stream {

// =====wjy====
enum class StreamStatusCode : int {
    Idle = FSREMOTE_STATUS_IDLE,
    ConnectingTcp = FSREMOTE_STATUS_CONNECTING_TCP,
    TcpConnected = FSREMOTE_STATUS_TCP_CONNECTED,
    InitializingWebrtc = FSREMOTE_STATUS_INITIALIZING_WEBRTC,
    WaitingRemoteStream = FSREMOTE_STATUS_WAITING_REMOTE_STREAM,
    ReceivingVideo = FSREMOTE_STATUS_RECEIVING_VIDEO,
    VideoStats = FSREMOTE_STATUS_VIDEO_STATS,
    MouseMode = FSREMOTE_STATUS_MOUSE_MODE,
    QualityApplied = FSREMOTE_STATUS_QUALITY_APPLIED,
    CursorShape = FSREMOTE_STATUS_CURSOR_SHAPE, // wjy: 强类型状态枚举同步公开 DLL 的远端光标形状通知。
    MouseBackend = FSREMOTE_STATUS_MOUSE_BACKEND, // wjy: Qt 强类型状态同步 Host 全局鼠标注入后端确认与故障回退。
    Admitted = FSREMOTE_STATUS_ADMITTED,
    ViewOnly = FSREMOTE_STATUS_VIEW_ONLY,
    ControlGranted = FSREMOTE_STATUS_CONTROL_GRANTED,
    ControlRequestPending = FSREMOTE_STATUS_CONTROL_REQUEST_PENDING,
    ControlRevoked = FSREMOTE_STATUS_CONTROL_REVOKED,
    RemoteClosed = FSREMOTE_STATUS_REMOTE_CLOSED,
    NetworkUnstable = FSREMOTE_STATUS_NETWORK_UNSTABLE, // wjy: Qt层可区分短时ICE波动与已经终止的连接。
    NetworkRecovering = FSREMOTE_STATUS_NETWORK_RECOVERING, // wjy: ICE恢复后仍以首个成功呈现帧作为最终恢复条件。
    Error = FSREMOTE_STATUS_ERROR,
    CapacityRejected = FSREMOTE_STATUS_CAPACITY_REJECTED,
    AuthorizationRejected = FSREMOTE_STATUS_AUTHORIZATION_REJECTED,
    IncompatibleProtocol = FSREMOTE_STATUS_INCOMPATIBLE_PROTOCOL,
}; // wjy: Qt 层使用强类型状态码，后续窗口状态机不再直接比较整数。
// ===end====

class StreamRuntime final {
public:
    static StreamRuntime& instance();

    bool isLoaded() const;
    QString lastError() const;

    FsRemoteStreamHandle startHost(uint16_t port);
    FsRemoteStreamHandle startHost(uint16_t port, const FsRemoteHostConfig& config);
    FsRemoteStreamHandle startViewer(
        const QString& hostIp,
        uint16_t port,
        FsRemoteFrameCallback callback,
        void* user);
    FsRemoteStreamHandle startViewer(
        const QString& hostIp,
        uint16_t port,
        FsRemoteFrameCallback frameCallback,
        FsRemoteStatusCallback statusCallback,
        void* user);
    FsRemoteStreamHandle startViewer(
        const QString& hostIp,
        uint16_t port,
        FsRemoteFrameCallback frameCallback,
        FsRemoteTextureFrameCallback textureCallback,
        FsRemoteStatusCallback statusCallback,
        void* user,
        bool monitorReadOnly = false); // wjy: 监控窗口必须走DLL只读准入入口；普通远控继续兼容旧版纹理Viewer导出。
    void stop(FsRemoteStreamHandle handle);
    bool sendInput(FsRemoteStreamHandle handle, const QByteArray& message);
    bool setViewerQuality(FsRemoteStreamHandle handle, const FsRemoteViewerQualityConfig& config); // wjy: 新DLL在线发送质量请求，旧DLL缺失导出时返回false但不停止当前流。
    bool setViewerAudioEnabled(FsRemoteStreamHandle handle, bool enabled); // wjy: 可选在线音频导出缺失时返回false，画面和控制继续保持连接。
    bool viewerQualityStatus(FsRemoteStreamHandle handle, FsRemoteViewerQualityStatus* status) const; // wjy: 读取Host实际应用结果供标题栏反馈。
    bool viewerPerformanceStats(FsRemoteStreamHandle handle, FsRemoteViewerPerformanceStats* stats) const; // wjy: 可选读取接收端性能累计量；旧 DLL 无导出时返回 false 且绝不把低 FPS 猜成压力。
    bool isBusy(FsRemoteStreamHandle handle) const;
    // =====wjy====
    uint32_t activeSessionCount(FsRemoteStreamHandle handle) const; // wjy: 查询主机当前活动远控会话数，驱动设备行人数徽标。
    QString activeControllerNames(FsRemoteStreamHandle handle) const; // wjy: 逗号分隔控制端设备名，供状态气泡展示。
    QString activeControllerDetails(FsRemoteStreamHandle handle) const; // wjy: 每行“设备名\tIP”，供目标端右下角远控提示层展示。
    // ===end====

private:
    StreamRuntime();

    using StartHostFn = FsRemoteStreamHandle(FSREMOTE_STREAM_CALL*)(uint16_t);
    using SetIdentityCallbacksFn = void(FSREMOTE_STREAM_CALL*)(const FsRemoteIdentityCallbacks*);
    using StartHostWithConfigFn = FsRemoteStreamHandle(FSREMOTE_STREAM_CALL*)(uint16_t, const FsRemoteHostConfig*);
    using StartViewerFn = FsRemoteStreamHandle(FSREMOTE_STREAM_CALL*)(
        const char*,
        uint16_t,
        FsRemoteFrameCallback,
        void*);
    using StartViewerWithStatusFn = FsRemoteStreamHandle(FSREMOTE_STREAM_CALL*)(
        const char*,
        uint16_t,
        FsRemoteFrameCallback,
        FsRemoteStatusCallback,
        void*);
    using StartViewerWithTextureFn = FsRemoteStreamHandle(FSREMOTE_STREAM_CALL*)(
        const char*,
        uint16_t,
        FsRemoteFrameCallback,
        FsRemoteTextureFrameCallback,
        FsRemoteStatusCallback,
        void*);
    // =====wjy====
    using StartViewerWithTextureRoleFn = FsRemoteStreamHandle(FSREMOTE_STREAM_CALL*)(
        const char*,
        uint16_t,
        FsRemoteFrameCallback,
        FsRemoteTextureFrameCallback,
        FsRemoteStatusCallback,
        void*,
        FsRemoteViewerRole); // wjy: 角色参数由DLL同步复制，Viewer工作线程不读取Qt窗口临时状态。
    // ===end====
    using StopFn = void(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle);
    using SendInputFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, const char*);
    using SetViewerQualityFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, const FsRemoteViewerQualityConfig*);
    using SetViewerAudioEnabledFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, int);
    using GetViewerQualityStatusFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, FsRemoteViewerQualityStatus*);
    using GetViewerPerformanceStatsFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, FsRemoteViewerPerformanceStats*);
    using IsBusyFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle);
    // =====wjy====
    using ActiveSessionCountFn = uint32_t(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle);
    using ActiveControllerNamesFn = uint32_t(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, char*, uint32_t);
    using ActiveControllerDetailsFn = uint32_t(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, char*, uint32_t);
    // ===end====
    using LastErrorFn = const char*(FSREMOTE_STREAM_CALL*)();

    bool m_loaded = false;
    QString m_error;
    StartHostFn m_startHost = nullptr;
    SetIdentityCallbacksFn m_setIdentityCallbacks = nullptr;
    StartHostWithConfigFn m_startHostWithConfig = nullptr;
    StartViewerFn m_startViewer = nullptr;
    StartViewerWithStatusFn m_startViewerWithStatus = nullptr;
    StartViewerWithTextureFn m_startViewerWithTexture = nullptr;
    StartViewerWithTextureRoleFn m_startViewerWithTextureRole = nullptr; // wjy: 缺失时普通远控可回退，监控不得降级为占名额的控制会话。
    StopFn m_stop = nullptr;
    SendInputFn m_sendInput = nullptr;
    SetViewerQualityFn m_setViewerQuality = nullptr;
    SetViewerAudioEnabledFn m_setViewerAudioEnabled = nullptr;
    GetViewerQualityStatusFn m_getViewerQualityStatus = nullptr;
    GetViewerPerformanceStatsFn m_getViewerPerformanceStats = nullptr;
    IsBusyFn m_isBusy = nullptr;
    // =====wjy====
    ActiveSessionCountFn m_activeSessionCount = nullptr;
    ActiveControllerNamesFn m_activeControllerNames = nullptr;
    ActiveControllerDetailsFn m_activeControllerDetails = nullptr;
    // ===end====
    LastErrorFn m_lastError = nullptr;
};

} // namespace stream
