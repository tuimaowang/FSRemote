#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace lsp {

bool parse_packet(const uint8_t* data, size_t size, PacketView* out)
{
    if (size < sizeof(PacketHeader)) {
        return false;
    }
    PacketHeader header = {};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != kPacketMagic || header.version != 1) {
        return false;
    }
    if (sizeof(PacketHeader) + header.payload_size > size) {
        return false;
    }
    out->header = header;
    out->payload = data + sizeof(PacketHeader);
    return true;
}

bool parse_feedback(const uint8_t* data, size_t size, FeedbackHeader* out)
{
    if (size < sizeof(FeedbackHeader)) {
        return false;
    }
    FeedbackHeader header = {};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != kFeedbackMagic || header.version != 1) {
        return false;
    }
    *out = header;
    return true;
}

std::vector<uint8_t> make_keyframe_feedback(uint32_t frame_id)
{
    FeedbackHeader header;
    header.frame_id = frame_id;
    std::vector<uint8_t> packet(sizeof(header));
    std::memcpy(packet.data(), &header, sizeof(header));
    return packet;
}

std::vector<uint8_t> make_nack_feedback(uint32_t frame_id, uint16_t fragment_index)
{
    FeedbackHeader header;
    header.type = kFeedbackNackFragment;
    header.frame_id = frame_id;
    header.fragment_index = fragment_index;
    std::vector<uint8_t> packet(sizeof(header));
    std::memcpy(packet.data(), &header, sizeof(header));
    return packet;
}

std::vector<std::vector<uint8_t>> fragment_frame(const std::vector<uint8_t>& frame, uint32_t frame_id,
                                                 uint64_t timestamp_us, Size size, bool keyframe)
{
    const uint16_t count = static_cast<uint16_t>((frame.size() + kMaxPayload - 1) / kMaxPayload);
    std::vector<std::vector<uint8_t>> packets;
    packets.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t offset = size_t(i) * kMaxPayload;
        const size_t payload_size = std::min(kMaxPayload, frame.size() - offset);
        PacketHeader header;
        header.flags = keyframe ? kFlagKeyFrame : 0;
        header.frame_id = frame_id;
        header.timestamp_us = timestamp_us;
        header.width = static_cast<uint16_t>(std::min(size.width, 65535u));
        header.height = static_cast<uint16_t>(std::min(size.height, 65535u));
        header.frame_size = static_cast<uint32_t>(frame.size());
        header.fragment_index = i;
        header.fragment_count = count;
        header.payload_size = static_cast<uint16_t>(payload_size);

        std::vector<uint8_t> packet(sizeof(PacketHeader) + payload_size);
        std::memcpy(packet.data(), &header, sizeof(header));
        std::memcpy(packet.data() + sizeof(header), frame.data() + offset, payload_size);
        packets.push_back(std::move(packet));
    }
    for (uint16_t group_start = 0; group_start < count; group_start += kFecGroupSize) {
        const uint16_t group_end = std::min<uint16_t>(count, group_start + kFecGroupSize);
        std::vector<uint8_t> parity(kMaxPayload, 0);
        uint16_t parity_size = 0;
        for (uint16_t i = group_start; i < group_end; ++i) {
            const size_t offset = size_t(i) * kMaxPayload;
            const size_t payload_size = std::min(kMaxPayload, frame.size() - offset);
            parity_size = std::max<uint16_t>(parity_size, static_cast<uint16_t>(payload_size));
            for (size_t j = 0; j < payload_size; ++j) {
                parity[j] ^= frame[offset + j];
            }
        }

        PacketHeader header;
        header.flags = static_cast<uint16_t>((keyframe ? kFlagKeyFrame : 0) | kFlagFec);
        header.frame_id = frame_id;
        header.timestamp_us = timestamp_us;
        header.width = static_cast<uint16_t>(std::min(size.width, 65535u));
        header.height = static_cast<uint16_t>(std::min(size.height, 65535u));
        header.frame_size = static_cast<uint32_t>(frame.size());
        header.fragment_index = group_start;
        header.fragment_count = count;
        header.payload_size = parity_size;

        std::vector<uint8_t> packet(sizeof(PacketHeader) + parity_size);
        std::memcpy(packet.data(), &header, sizeof(header));
        std::memcpy(packet.data() + sizeof(header), parity.data(), parity_size);
        packets.push_back(std::move(packet));
    }
    return packets;
}

} // namespace lsp
