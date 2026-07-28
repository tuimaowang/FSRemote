#include "uu_profile.h"

namespace uu {

const VideoPayloadProfile& video_payloads()
{
    static const VideoPayloadProfile profile;
    return profile;
}

const RtpExtensionProfile& rtp_extensions()
{
    static const RtpExtensionProfile profile;
    return profile;
}

std::string_view target_summary()
{
    return "UU target: WebRTC HEVC/H264 + RTX + RED/ULPFEC/FlexFEC + TWCC + REMB + NACK/PLI/FIR + hardware encode/decode";
}

std::string_view field_trials()
{
    // Confirmed in streamer.dll at string RVA 0xdd61d0.
    return "WebRTC-FrameLatency/Enabled/"
           "WebRTC-Pacer-FastRetransmissions/Enabled/"
           "WebRTC-Paced-Sender-02/Enabled/"
           "WebRTC-VideoCaptureIndex/Enabled/"
           "WebRTC-VideoIsNewFrame/Enabled/"
           "WebRTC-FeedbackLatency/Enabled/"
           "WebRTC-VideoFrameSendingDelay/Enabled/"
           "WebRTC-SendDelay-CountWithFirstPacket/Enabled/"
           // =====wjy====
           "WebRTC-PcFactoryDefaultBitrates/min:300,start:60000,max:240000/" // wjy: 旧配置把整个 PeerConnection 的发送上限锁在 8.1 Mbps，高质量后续即使请求 120 Mbps 也无法突破；默认改为 60 Mbps 起步、240 Mbps 硬上限，具体档位再通过 SetBitrate 收紧。
           // ===end====
           "WebRTC-ProbingScreenshareBwe/1.6,6,85,95,-60,3/"
           "WebRTC-VideoRateControl/probe_max_allocation:true/"
           "WebRTC-BweRapidRecoveryExperiment/Enabled/"
           "WebRTC-DecoderFrameMemoryLength/max_decoder_frame_memory_length:-1/"
           "WebRTC-Stats-VideoEndOfPacket/Enabled/"
           "WebRTC-VideoPlayoutDelayZero/Enabled/"
           "WebRTC-FlexFEC-03-Advertised/Enabled/"
           "WebRTC-DontIncreaseDelayBasedBweInAlr/Disabled/"
           "WebRTC-FlexFEC-03/Enabled/"
           "WebRTC-ProtectionMinThreshold/0.10/"
           "WebRTC-ProtectionMaxThreshold/0.35/"
           "WebRTC-RttTimeoutMsThreshold/1000/"
           "WebRTC-UseTurnServerAsStunServer/Disabled/"
           "WebRTC-FrameDropper/Disabled/"
           "WebRTC-RemoteCandidatFilterPrflxType/Disabled/"
           "WebRTC-PacketBufferMaxSize/16384/"
           "WebRTC-IceFieldTrials/stop_gather_on_strongly_connected:false/"
           "WebRTC-AddPacingToCongestionWindowPushback/Enabled/"
           "WebRTC-AddNetworkCostToVpn/Enabled/"
           "WebRTC-NetworkMonitorAutoDetect/getAllNetworksFromCache:true,requestVPN:true/"
           "WebRTC-LinkPressureDetection/Enabled/"
           "WebRTC-KeyframeInterval/min_keyframe_send_interval_ms:600/"
           "WebRTC-RetransmissionRateLimit/Enabled/"
           "WebRTC-Pacer-Task-Queue-WinAdvance/Enabled/";
}

} // namespace uu
