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

// =====wjy====
enum FsRemoteStreamStatusCode {
    FSREMOTE_STATUS_IDLE = 0,
    FSREMOTE_STATUS_CONNECTING_TCP = 10,
    FSREMOTE_STATUS_TCP_CONNECTED = 20,
    FSREMOTE_STATUS_INITIALIZING_WEBRTC = 30,
    FSREMOTE_STATUS_WAITING_REMOTE_STREAM = 40,
    FSREMOTE_STATUS_RECEIVING_VIDEO = 50,
    FSREMOTE_STATUS_VIDEO_STATS = 60,
    FSREMOTE_STATUS_MOUSE_MODE = 61,
    FSREMOTE_STATUS_ADMITTED = 70,
    FSREMOTE_STATUS_VIEW_ONLY = 71,
    FSREMOTE_STATUS_CONTROL_GRANTED = 72,
    FSREMOTE_STATUS_CONTROL_REQUEST_PENDING = 73,
    FSREMOTE_STATUS_CONTROL_REVOKED = 74,
    FSREMOTE_STATUS_REMOTE_CLOSED = 80,
    FSREMOTE_STATUS_ERROR = 90,
    FSREMOTE_STATUS_CAPACITY_REJECTED = 91,
    FSREMOTE_STATUS_AUTHORIZATION_REJECTED = 92,
    FSREMOTE_STATUS_INCOMPATIBLE_PROTOCOL = 93,
}; // wjy: 稳定状态码同时服务 DLL 和 Qt，避免多控制端状态继续依赖散落的魔法数字。

enum FsRemoteOwnershipPolicy {
    FSREMOTE_OWNERSHIP_EXCLUSIVE = 1,
};

typedef struct FsRemoteHostConfig {
    uint32_t struct_size; // wjy: 调用方填写结构体大小，使未来追加字段时仍能兼容旧二进制。
    uint32_t version; // wjy: 当前主机配置结构版本固定为 1。
    uint32_t max_sessions; // wjy: 请求的并发会话上限；迁移阶段 DLL 内部仍强制有效值为 1。
    uint32_t max_aggregate_video_kbps; // wjy: 所有发送会话共享的视频码率预算。
    uint32_t handshake_timeout_ms; // wjy: 未认证连接完成准入握手的最长时间。
    uint32_t ownership_policy; // wjy: 首版仅支持单控制权独占策略。
} FsRemoteHostConfig;

typedef uint32_t(FSREMOTE_STREAM_CALL* FsRemoteReadPublicKeyCallback)(void* user, uint8_t* output, uint32_t output_capacity);
typedef uint32_t(FSREMOTE_STREAM_CALL* FsRemoteSignChallengeCallback)(void* user, const uint8_t* challenge, uint32_t challenge_size, uint8_t* output, uint32_t output_capacity);
typedef int(FSREMOTE_STREAM_CALL* FsRemoteIsPublicKeyAuthorizedCallback)(void* user, const uint8_t* public_key, uint32_t public_key_size);
typedef int(FSREMOTE_STREAM_CALL* FsRemoteVerifyChallengeCallback)(void* user, const uint8_t* public_key, uint32_t public_key_size, const uint8_t* challenge, uint32_t challenge_size, const uint8_t* signature, uint32_t signature_size);

typedef struct FsRemoteIdentityCallbacks {
    uint32_t struct_size;
    uint32_t version;
    void* user; // wjy: 回调上下文由 Qt 主程序持有，生命周期覆盖 DLL 内全部主机和 Viewer 工作线程。
    FsRemoteReadPublicKeyCallback read_public_key;
    FsRemoteSignChallengeCallback sign_challenge;
    FsRemoteIsPublicKeyAuthorizedCallback is_public_key_authorized;
    FsRemoteVerifyChallengeCallback verify_challenge;
} FsRemoteIdentityCallbacks; // wjy: DLL 不直接依赖 Qt/OpenSSH 路径，通过稳定回调桥接现有设备密钥能力。
// ===end====

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host(uint16_t port);
void FSREMOTE_STREAM_CALL fsremote_stream_set_identity_callbacks(const FsRemoteIdentityCallbacks* callbacks);
FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host_with_config(uint16_t port, const FsRemoteHostConfig* config);
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
