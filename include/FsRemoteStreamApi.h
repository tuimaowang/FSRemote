#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define FSREMOTE_STREAM_CALL __stdcall
#else
#define FSREMOTE_STREAM_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void(FSREMOTE_STREAM_CALL* FsRemoteFrameCallback)(
    void* user,
    int width,
    int height,
    const uint8_t* bgra,
    uint32_t bgra_size);

typedef void(FSREMOTE_STREAM_CALL* FsRemoteStatusCallback)(
    void* user,
    int status_code,
    const char* message);

typedef int(FSREMOTE_STREAM_CALL* FsRemoteTextureFrameCallback)(
    void* user,
    int width,
    int height,
    void* shared_handle,
    uint64_t frame_id,
    double encoded_mbps);

typedef void* FsRemoteStreamHandle;

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host(uint16_t port);
FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback callback,
    void* user);
FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer_with_status(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback frame_callback,
    FsRemoteStatusCallback status_callback,
    void* user);
FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer_with_texture(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback frame_callback,
    FsRemoteTextureFrameCallback texture_callback,
    FsRemoteStatusCallback status_callback,
    void* user);
void FSREMOTE_STREAM_CALL fsremote_stream_stop(FsRemoteStreamHandle handle);
int FSREMOTE_STREAM_CALL fsremote_stream_send_input(FsRemoteStreamHandle handle, const char* message);
int FSREMOTE_STREAM_CALL fsremote_stream_is_busy(FsRemoteStreamHandle handle);
const char* FSREMOTE_STREAM_CALL fsremote_stream_last_error(void);

#ifdef __cplusplus
}
#endif
