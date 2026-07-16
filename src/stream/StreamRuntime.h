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
    Admitted = FSREMOTE_STATUS_ADMITTED,
    ViewOnly = FSREMOTE_STATUS_VIEW_ONLY,
    ControlGranted = FSREMOTE_STATUS_CONTROL_GRANTED,
    ControlRequestPending = FSREMOTE_STATUS_CONTROL_REQUEST_PENDING,
    ControlRevoked = FSREMOTE_STATUS_CONTROL_REVOKED,
    RemoteClosed = FSREMOTE_STATUS_REMOTE_CLOSED,
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
        void* user);
    void stop(FsRemoteStreamHandle handle);
    bool sendInput(FsRemoteStreamHandle handle, const QByteArray& message);
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
    using StopFn = void(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle);
    using SendInputFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, const char*);
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
    StopFn m_stop = nullptr;
    SendInputFn m_sendInput = nullptr;
    IsBusyFn m_isBusy = nullptr;
    // =====wjy====
    ActiveSessionCountFn m_activeSessionCount = nullptr;
    ActiveControllerNamesFn m_activeControllerNames = nullptr;
    ActiveControllerDetailsFn m_activeControllerDetails = nullptr;
    // ===end====
    LastErrorFn m_lastError = nullptr;
};

} // namespace stream
