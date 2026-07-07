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
using DecodedTextureCallback = std::function<bool(int width, int height, void* shared_handle, uint64_t frame_id, double encoded_mbps)>;

std::unique_ptr<webrtc::VideoEncoderFactory> CreateUuVideoEncoderFactory();
std::unique_ptr<webrtc::VideoDecoderFactory> CreateUuVideoDecoderFactory(DecodedBgraCallback bgra_callback = {}, DecodedTextureCallback texture_callback = {});
void SetUuDecodedFrameHook(std::function<void(const webrtc::VideoFrame&)> hook);
void SetUuDecodedBgraHook(DecodedBgraCallback hook);

} // namespace uu
