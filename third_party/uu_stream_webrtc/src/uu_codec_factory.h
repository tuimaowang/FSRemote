#pragma once

#include <api/video/video_frame.h>
#include <api/video_codecs/video_decoder_factory.h>
#include <api/video_codecs/video_encoder_factory.h>

#include <functional>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace uu {

using DecodedBgraCallback = std::function<void(int width, int height, const uint8_t* bgra, size_t size, double encoded_mbps)>;

// =====wjy====
enum DecodedTextureFrameResult {
    DecodedTextureFallback = 0, // wjy: 纹理无法安全交付时执行BGRA软件回读。
    DecodedTextureAccepted = 1, // wjy: Qt已接受共享纹理，解码器把keyed mutex交给消费者。
    DecodedTextureDropped = 2, // wjy: Qt单槽繁忙时受控丢帧，跳过BGRA回读并立即复用生产者纹理。
};
// ===end====

using DecodedTextureCallback = std::function<int(int width, int height, void* shared_handle, uint64_t frame_id, double encoded_mbps)>; // wjy: 0表示软件回退，1表示GPU接管，2表示受控丢帧并立即归还共享纹理。

std::unique_ptr<webrtc::VideoEncoderFactory> CreateUuVideoEncoderFactory();
std::unique_ptr<webrtc::VideoDecoderFactory> CreateUuVideoDecoderFactory(DecodedBgraCallback bgra_callback = {}, DecodedTextureCallback texture_callback = {});
void SetUuDecodedFrameHook(std::function<void(const webrtc::VideoFrame&)> hook);
void SetUuDecodedBgraHook(DecodedBgraCallback hook);

} // namespace uu
