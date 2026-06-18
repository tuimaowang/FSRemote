#pragma once

#include "FsRemoteStreamApi.h"

#include <QByteArray>
#include <QString>

namespace stream {

class StreamRuntime final {
public:
    static StreamRuntime& instance();

    bool isLoaded() const;
    QString lastError() const;

    FsRemoteStreamHandle startHost(uint16_t port);
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
    void stop(FsRemoteStreamHandle handle);
    bool sendInput(FsRemoteStreamHandle handle, const QByteArray& message);
    bool isBusy(FsRemoteStreamHandle handle) const;

private:
    StreamRuntime();

    using StartHostFn = FsRemoteStreamHandle(FSREMOTE_STREAM_CALL*)(uint16_t);
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
    using StopFn = void(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle);
    using SendInputFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle, const char*);
    using IsBusyFn = int(FSREMOTE_STREAM_CALL*)(FsRemoteStreamHandle);
    using LastErrorFn = const char*(FSREMOTE_STREAM_CALL*)();

    bool m_loaded = false;
    QString m_error;
    StartHostFn m_startHost = nullptr;
    StartViewerFn m_startViewer = nullptr;
    StartViewerWithStatusFn m_startViewerWithStatus = nullptr;
    StopFn m_stop = nullptr;
    SendInputFn m_sendInput = nullptr;
    IsBusyFn m_isBusy = nullptr;
    LastErrorFn m_lastError = nullptr;
};

} // namespace stream
