/**
 * Copyright (c) 2025 Jacob Sologub
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "twccfeedbackparser.hpp"
#include "internals.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <intrin.h> // For __popcnt
#else
#include <arpa/inet.h>
#endif

namespace rtc::impl {

std::optional<TwccFeedbackParser::ParsedFeedback> TwccFeedbackParser::parse(const RtcpTwcc* twcc, size_t totalSize) {
    if (!twcc) {
        return std::nullopt;
    }
    
    ParsedFeedback feedback;
    feedback.baseSeqNum = twcc->getBaseSeqNum();
    feedback.referenceTime = twcc->getReferenceTime();
    feedback.feedbackPacketCount = twcc->getFbPacketCount();
    
    // Reference time is in 64ms units, convert to microseconds
    feedback.baseTime = std::chrono::microseconds(feedback.referenceTime * 64000);
    
    uint16_t packetCount = twcc->getPacketStatusCount();
    if (packetCount == 0) {
        return feedback; // Empty feedback is valid
    }
    
    const uint8_t* data = reinterpret_cast<const uint8_t*>(twcc->getBody());
    size_t dataSize = totalSize - sizeof(RtcpTwcc);
    size_t offset = 0;
    
    // Parse packet status chunks
    std::vector<PacketStatus::Status> statuses;
    statuses.reserve(packetCount);
    
    while (statuses.size() < packetCount && offset < dataSize) {
        if (!parseStatusChunk(data, offset, dataSize, statuses)) {
            PLOG_WARNING << "TWCC: Failed to parse status chunk";
            return std::nullopt;
        }
    }
    
    // Trim to exact packet count
    if (statuses.size() > packetCount) {
        statuses.resize(packetCount);
    }
    
    // Parse receive deltas
    std::vector<std::chrono::microseconds> deltas;
    if (!parseReceiveDeltas(data, offset, dataSize, statuses, deltas)) {
        PLOG_WARNING << "TWCC: Failed to parse receive deltas";
        return std::nullopt;
    }
    
    // Combine into packet status
    uint16_t seqNum = feedback.baseSeqNum;
    for (size_t i = 0; i < statuses.size(); ++i) {
        PacketStatus packet;
        packet.seqNum = seqNum++;
        packet.status = statuses[i];

        if (i < deltas.size()) {
            packet.receiveDelta = deltas[i];
        } else {
            packet.receiveDelta = std::chrono::microseconds(0);
        }

        feedback.packets.push_back(packet);
    }
    
    return feedback;
}

bool TwccFeedbackParser::parseStatusChunk(const uint8_t* data, size_t& offset, size_t maxSize,
                                          std::vector<PacketStatus::Status>& statuses) {
    if (offset >= maxSize) {
        return false;
    }
    
    if (offset + 1 >= maxSize) {
        return false;
    }

    uint16_t chunk = (data[offset] << 8) | data[offset + 1];
    offset += 2;

    if ((chunk & 0x8000) == 0) {
        // Run-length chunk: T=0, 2 bits status, 13 bits run length
        PacketStatus::Status status = static_cast<PacketStatus::Status>((chunk >> 13) & 0x03);
        uint16_t runLength = chunk & 0x1FFF;

        // Reject invalid status=3 per Google WebRTC
        if (status == PacketStatus::Reserved) {
            PLOG_WARNING << "TWCC: Invalid run-length status=3 chunk=0x"
                         << std::hex << chunk << std::dec;
            return false;
        }

        for (uint16_t i = 0; i < runLength; ++i) {
            statuses.push_back(status);
        }
    } else if ((chunk & 0x4000) == 0) {
        // One-bit status vector: T=1, S=0, 14 one-bit symbols
        // Per Google WebRTC: Read from MSB to LSB (bit 13 down to 0)
        for (int i = 0; i < 14; ++i) {
            bool received = (chunk >> (13 - i)) & 0x01;
            statuses.push_back(received ? PacketStatus::ReceivedSmallDelta
                                        : PacketStatus::NotReceived);
        }
    } else {
        // Two-bit status vector: T=1, S=1, 7 two-bit symbols
        // Per Google WebRTC: Read from MSB to LSB
        for (int i = 0; i < 7; ++i) {
            uint8_t status = (chunk >> (2 * (6 - i))) & 0x03;
            statuses.push_back(static_cast<PacketStatus::Status>(status));
        }
    }
    
    return true;
}

bool TwccFeedbackParser::parseReceiveDeltas(const uint8_t* data, size_t& offset, size_t maxSize,
                                            const std::vector<PacketStatus::Status>& statuses,
                                            std::vector<std::chrono::microseconds>& deltas) {
    std::chrono::microseconds accumulator(0);
    
    for (auto status : statuses) {
        if (status == PacketStatus::ReceivedSmallDelta) {
            // Small delta: 1 byte UNSIGNED, in 250us units (0-63.75ms per RFC 8888)
            if (offset >= maxSize) {
                PLOG_WARNING << "TWCC: Unexpected end of receive deltas";
                return false;
            }

            uint8_t delta = data[offset++];
            accumulator += std::chrono::microseconds(delta * 250);
            deltas.push_back(accumulator);
            
        } else if (status == PacketStatus::ReceivedLargeDelta) {
            // Large delta: 2 bytes SIGNED, in 250us units (-8192ms to 8191.75ms per RFC 8888)
            if (offset + 2 > maxSize) {
                PLOG_WARNING << "TWCC: Unexpected end of receive deltas";
                return false;
            }

            uint16_t raw = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
            int16_t delta = static_cast<int16_t>(raw);
            offset += 2;

            accumulator += std::chrono::microseconds(delta * 250);
            // Note: Negative accumulator is valid for reordered packets
            deltas.push_back(accumulator);
            
        } else if (status == PacketStatus::NotReceived) {
            // No delta for lost packets
            deltas.push_back(std::chrono::microseconds(0));
        }
    }
    
    return true;
}

} // namespace rtc::impl

#endif /* RTC_ENABLE_MEDIA */
