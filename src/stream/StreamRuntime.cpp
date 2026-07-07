#include "stream/StreamRuntime.h"

#include <QCoreApplication>
#include <QLibrary>

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
    m_startViewer = reinterpret_cast<StartViewerFn>(library->resolve("fsremote_stream_start_viewer"));
    m_startViewerWithStatus = reinterpret_cast<StartViewerWithStatusFn>(library->resolve("fsremote_stream_start_viewer_with_status"));
    m_startViewerWithTexture = reinterpret_cast<StartViewerWithTextureFn>(library->resolve("fsremote_stream_start_viewer_with_texture"));
    m_stop = reinterpret_cast<StopFn>(library->resolve("fsremote_stream_stop"));
    m_sendInput = reinterpret_cast<SendInputFn>(library->resolve("fsremote_stream_send_input"));
    m_isBusy = reinterpret_cast<IsBusyFn>(library->resolve("fsremote_stream_is_busy"));
    m_lastError = reinterpret_cast<LastErrorFn>(library->resolve("fsremote_stream_last_error"));
    m_loaded = m_startHost && m_startViewer && m_stop && m_lastError;
    if (!m_loaded) {
        m_error = QStringLiteral("fsremote_stream.dll exports are incomplete");
    }
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
    void* user)
{
    const QByteArray ip = hostIp.toUtf8();
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

bool StreamRuntime::isBusy(FsRemoteStreamHandle handle) const
{
    return m_isBusy && handle && m_isBusy(handle) != 0;
}

} // namespace stream
