#pragma once

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lsp {

constexpr uint32_t kPacketMagic = 0x4C535031; // LSP1
constexpr uint32_t kFeedbackMagic = 0x4C464231; // LFB1
constexpr size_t kMaxPayload = 1400;
constexpr uint16_t kFlagKeyFrame = 1u << 0;
constexpr uint16_t kFlagFec = 1u << 1;
constexpr uint16_t kFeedbackRequestKeyFrame = 1u;
constexpr uint16_t kFeedbackNackFragment = 2u;
constexpr uint16_t kFecGroupSize = 2;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic = kPacketMagic;
    uint16_t version = 1;
    uint16_t flags = 0;
    uint32_t frame_id = 0;
    uint64_t timestamp_us = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t frame_size = 0;
    uint16_t fragment_index = 0;
    uint16_t fragment_count = 0;
    uint16_t payload_size = 0;
};

struct FeedbackHeader {
    uint32_t magic = kFeedbackMagic;
    uint16_t version = 1;
    uint16_t type = kFeedbackRequestKeyFrame;
    uint32_t frame_id = 0;
    uint16_t fragment_index = 0;
    uint16_t reserved = 0;
};
#pragma pack(pop)

struct PacketView {
    PacketHeader header;
    const uint8_t* payload = nullptr;
};

bool parse_packet(const uint8_t* data, size_t size, PacketView* out);
bool parse_feedback(const uint8_t* data, size_t size, FeedbackHeader* out);
std::vector<uint8_t> make_keyframe_feedback(uint32_t frame_id);
std::vector<uint8_t> make_nack_feedback(uint32_t frame_id, uint16_t fragment_index);
std::vector<std::vector<uint8_t>> fragment_frame(const std::vector<uint8_t>& frame, uint32_t frame_id,
                                                 uint64_t timestamp_us, Size size, bool keyframe);

} // namespace lsp
