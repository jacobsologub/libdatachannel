/**
 * Copyright (c) 2025 Jacob Sologub
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_RTP_TWCC_HANDLER_H
#define RTC_RTP_TWCC_HANDLER_H

#if RTC_ENABLE_MEDIA

#include "mediahandler.hpp"
#include "rtp.hpp"
#include "twccstats.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rtc {

namespace impl {
    class TwccPacketTracker;
    class TwccFeedbackParser;
}

// Handler for adding Transport-wide Congestion Control sequence numbers to RTP packets
class RTC_CPP_EXPORT RtpTwccHandler final : public MediaHandler {
public:
    // Parsed TWCC feedback packet information
    struct PacketInfo {
        uint16_t seqNum;
        bool received;
        std::chrono::microseconds receiveDelta; // Delta from reference time
    };

    struct ParsedFeedback {
        uint16_t baseSeqNum;
        uint32_t referenceTimeMs; // Reference time in milliseconds
        std::vector<PacketInfo> packets;
    };

    // Information about a sent packet
    struct SentPacketInfo {
        std::chrono::steady_clock::time_point sendTime;
        size_t size;
    };

    // extId: RTP extension ID negotiated for TWCC (typically 5)
    RtpTwccHandler(uint8_t extId);
    ~RtpTwccHandler();

    // Get statistics from the packet tracker
    TwccStats getStats() const;

    // Process TWCC feedback (returns packets that were acknowledged)
    void processFeedback(const RtcpTwcc* twcc);

    // Parse TWCC feedback message into structured data
    static std::optional<ParsedFeedback> parseTwccFeedback(const RtcpTwcc* twcc, size_t totalSize);

    // Get information about a sent packet by sequence number
    std::optional<SentPacketInfo> getSentPacketInfo(uint16_t seqNum) const;

    // Process outgoing RTP packets - adds TWCC sequence numbers
    void outgoing(message_vector &messages, const message_callback &send) override;
    
private:
    const uint8_t mTwccExtId;
    std::atomic<uint16_t> mTwccSeqNum{0};
    std::shared_ptr<impl::TwccPacketTracker> mPacketTracker;
    mutable std::mutex mSentPacketsMutex;
    std::unordered_map<uint16_t, SentPacketInfo> mSentPackets;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_RTP_TWCC_HANDLER_H */