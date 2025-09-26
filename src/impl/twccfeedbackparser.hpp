/**
 * Copyright (c) 2025 Jacob Sologub
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_TWCC_FEEDBACK_PARSER_H
#define RTC_IMPL_TWCC_FEEDBACK_PARSER_H

#include "common.hpp"

#if RTC_ENABLE_MEDIA

#include "rtp.hpp"
#include <chrono>
#include <vector>

namespace rtc::impl {

// Parses RTCP Transport-wide Congestion Control feedback messages
class TwccFeedbackParser {
public:
    struct PacketStatus {
        enum Status {
            NotReceived = 0,
            ReceivedSmallDelta = 1,
            ReceivedLargeDelta = 2,
            Reserved = 3
        };
        
        uint16_t seqNum;
        Status status;
        std::chrono::microseconds receiveDelta; // Delta from reference time
    };
    
    struct ParsedFeedback {
        uint16_t baseSeqNum;
        uint32_t referenceTime; // In 64ms units
        uint8_t feedbackPacketCount;
        std::vector<PacketStatus> packets;
        std::chrono::microseconds baseTime; // Reference time in microseconds
    };
    
    // Parse TWCC feedback message
    static std::optional<ParsedFeedback> parse(const RtcpTwcc* twcc, size_t totalSize);
    
private:
    // Parse status chunk (run-length or status vector)
    static bool parseStatusChunk(const uint8_t* data, size_t& offset, size_t maxSize,
                                 std::vector<PacketStatus::Status>& statuses);
    
    // Parse receive delta chunk
    static bool parseReceiveDeltas(const uint8_t* data, size_t& offset, size_t maxSize,
                                   const std::vector<PacketStatus::Status>& statuses,
                                   std::vector<std::chrono::microseconds>& deltas);
};

} // namespace rtc::impl

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_IMPL_TWCC_FEEDBACK_PARSER_H */
