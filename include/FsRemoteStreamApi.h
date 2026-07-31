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

// =====wjy====
enum FsRemoteTextureFrameResult {
    FSREMOTE_TEXTURE_FRAME_FALLBACK = 0, // wjy: 当前纹理无法进入安全GPU路径，解码器继续生成BGRA软件帧保证画面不中断。
    FSREMOTE_TEXTURE_FRAME_ACCEPTED = 1, // wjy: 控制端已经接管共享纹理，生产端将纹理所有权交给D3D11 Presenter。
    FSREMOTE_TEXTURE_FRAME_DROPPED = 2, // wjy: Qt单槽正在占用，主动丢弃当前帧且不做昂贵BGRA回读，生产端立即回收纹理槽。
};
// ===end====

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
    FSREMOTE_STATUS_QUALITY_APPLIED = 63,
    FSREMOTE_STATUS_CURSOR_SHAPE = 64, // wjy: Host 标准光标形状经现有状态回调交给 Qt，不改变 C ABI 函数签名。
    FSREMOTE_STATUS_MOUSE_BACKEND = 65, // wjy: 为兼容既有 ABI 保留名称；状态现在确认系统/FakerInputBridge 键鼠注入后端。
    FSREMOTE_STATUS_ADMITTED = 70,
    FSREMOTE_STATUS_VIEW_ONLY = 71,
    FSREMOTE_STATUS_CONTROL_GRANTED = 72,
    FSREMOTE_STATUS_CONTROL_REQUEST_PENDING = 73,
    FSREMOTE_STATUS_CONTROL_REVOKED = 74,
    FSREMOTE_STATUS_REMOTE_CLOSED = 80,
    FSREMOTE_STATUS_NETWORK_UNSTABLE = 81, // wjy: ICE暂时断开但会话仍存活，控制端先冻结输入并等待短时自恢复。
    FSREMOTE_STATUS_NETWORK_RECOVERING = 82, // wjy: ICE连接已恢复，继续等待真实视频帧确认画面和输入均可恢复。
    FSREMOTE_STATUS_ERROR = 90,
    FSREMOTE_STATUS_CAPACITY_REJECTED = 91,
    FSREMOTE_STATUS_AUTHORIZATION_REJECTED = 92,
    FSREMOTE_STATUS_INCOMPATIBLE_PROTOCOL = 93,
}; // wjy: 稳定状态码同时服务 DLL 和 Qt，避免多控制端状态继续依赖散落的魔法数字。

enum FsRemoteOwnershipPolicy {
    FSREMOTE_OWNERSHIP_EXCLUSIVE = 1,
    FSREMOTE_OWNERSHIP_SHARED = 2, // wjy: 默认协同策略允许所有已认证且协商 control 能力的会话同时发送键鼠输入。
};

typedef struct FsRemoteHostConfig {
    uint32_t struct_size; // wjy: 调用方填写结构体大小，使未来追加字段时仍能兼容旧二进制。
    uint32_t version; // wjy: 当前主机配置结构版本固定为 1。
    uint32_t max_sessions; // wjy: 实际并发视频会话上限，DLL 会把调用方配置夹紧到 1 至 3。
    uint32_t max_aggregate_video_kbps; // wjy: 所有发送会话共享的视频码率预算。
    uint32_t handshake_timeout_ms; // wjy: 未认证连接完成准入握手的最长时间。
    uint32_t ownership_policy; // wjy: 共享策略为默认值；保留独占枚举用于旧调用方 ABI 与后续兼容开关。
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

enum FsRemoteViewerQualityMode {
    FSREMOTE_VIEWER_QUALITY_AUTOMATIC = 1,
    FSREMOTE_VIEWER_QUALITY_HIGH_LOCKED = 2,
    FSREMOTE_VIEWER_QUALITY_BALANCED = 3,
    FSREMOTE_VIEWER_QUALITY_SMOOTH = 4,
};

enum FsRemoteViewerQualityLimitation {
    FSREMOTE_VIEWER_QUALITY_LIMIT_NONE = 0,
    FSREMOTE_VIEWER_QUALITY_LIMIT_UNSUPPORTED = 1,
    FSREMOTE_VIEWER_QUALITY_LIMIT_INVALID_REQUEST = 2,
    FSREMOTE_VIEWER_QUALITY_LIMIT_APPLY_FAILED = 3,
    FSREMOTE_VIEWER_QUALITY_LIMIT_CLAMPED = 4,
};

typedef struct FsRemoteViewerQualityConfig {
    uint32_t struct_size; // wjy: 调用方填写结构大小，未来版本追加字段时旧DLL可安全拒绝或只读取已知部分。
    uint32_t version; // wjy: 当前固定为1，与data-channel画质协议版本独立但同步演进。
    uint64_t request_id; // wjy: 单窗口单调递增，迟到确认不能覆盖更新的用户选择。
    uint32_t mode;
    uint32_t target_width; // wjy: 宽高同时为0表示原始分辨率；非0时由Host按固定档位应用。
    uint32_t target_height;
    uint32_t target_fps;
    uint32_t max_bitrate_kbps;
    uint32_t priority; // wjy: 0到100；高质量锁定优先降级更晚，但不能越过硬资源边界。
} FsRemoteViewerQualityConfig;

typedef struct FsRemoteViewerQualityStatus {
    uint32_t struct_size;
    uint32_t version;
    uint64_t request_id;
    uint32_t supported; // wjy: 0表示旧Host/超时/明确不支持，现有流继续运行。
    uint32_t applied_mode;
    uint32_t applied_width;
    uint32_t applied_height;
    uint32_t applied_fps;
    uint32_t applied_bitrate_kbps;
    uint32_t limitation;
} FsRemoteViewerQualityStatus; // wjy: Qt读取Host确认的实际值，标题栏不会把“请求值”冒充“已应用值”。

// =====wjy====
typedef struct FsRemoteViewerPerformanceStats {
    uint32_t struct_size; // wjy: 独立可选快照不扩大旧质量确认结构，旧 DLL 缺少导出时控制端继续按健康状态运行。
    uint32_t version; // wjy: 当前版本为 1，所有计数器均为 WebRTC 会话启动以来的累计值。
    uint64_t sample_time_ms;
    uint64_t frames_received;
    uint64_t frames_decoded;
    uint64_t frames_dropped;
    uint64_t freeze_count;
    uint64_t jitter_buffer_emitted_count;
    uint64_t packets_received;
    uint64_t packets_lost;
    double total_decode_time_ms;
    double total_processing_delay_ms;
    double total_freezes_duration_ms;
    double total_jitter_buffer_delay_ms;
    double round_trip_time_ms;
    double available_incoming_bitrate_kbps;
} FsRemoteViewerPerformanceStats; // wjy: 控制端对相邻快照求差，区分真实解码/网络压力与静止桌面自然低帧率。
// ===end====
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
// =====wjy====
int FSREMOTE_STREAM_CALL fsremote_stream_set_viewer_quality(FsRemoteStreamHandle handle, const FsRemoteViewerQualityConfig* config);
int FSREMOTE_STREAM_CALL fsremote_stream_set_viewer_audio_enabled(FsRemoteStreamHandle handle, int enabled); // wjy: 在线启停当前Viewer本地音频播放器，不重连视频、控制或认证会话。
int FSREMOTE_STREAM_CALL fsremote_stream_get_viewer_quality_status(FsRemoteStreamHandle handle, FsRemoteViewerQualityStatus* status);
int FSREMOTE_STREAM_CALL fsremote_stream_get_viewer_performance_stats(FsRemoteStreamHandle handle, FsRemoteViewerPerformanceStats* stats);
// ===end====
int FSREMOTE_STREAM_CALL fsremote_stream_is_busy(FsRemoteStreamHandle handle);
// =====wjy====
uint32_t FSREMOTE_STREAM_CALL fsremote_stream_active_session_count(FsRemoteStreamHandle handle); // wjy: 返回主机当前已登记会话数，供状态服务上报远控人数徽标。
// wjy: 复制当前控制端设备名（UTF-8，逗号分隔）到 output；返回需要的字节数（不含结尾 0）。output 为空或容量不足时只返回长度。
uint32_t FSREMOTE_STREAM_CALL fsremote_stream_active_controller_names(
    FsRemoteStreamHandle handle,
    char* output,
    uint32_t output_capacity);
// wjy: 复制当前控制端详情（每行“设备名\tIP”）到 output；目标端本机提示层用它显示是谁正在远控。
uint32_t FSREMOTE_STREAM_CALL fsremote_stream_active_controller_details(
    FsRemoteStreamHandle handle,
    char* output,
    uint32_t output_capacity);
// ===end====
const char* FSREMOTE_STREAM_CALL fsremote_stream_last_error(void);

#ifdef __cplusplus
}
#endif
