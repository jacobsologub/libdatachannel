/**
 * Copyright (c) 2025 Jacob Sologub
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_TWCC_PACKET_TRACKER_H
#define RTC_IMPL_TWCC_PACKET_TRACKER_H

#include "common.hpp"

#if RTC_ENABLE_MEDIA

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

namespace rtc::impl {

// Tracks packets for Transport-wide Congestion Control
class TwccPacketTracker {
public:
    struct PacketInfo {
        uint16_t seqNum;
        uint16_t size;
        std::chrono::steady_clock::time_point sentTime;
        std::optional<std::chrono::microseconds> receiveDelta; // From TWCC feedback
        bool received = false;
        
        PacketInfo(uint16_t seq, uint16_t sz) 
            : seqNum(seq), size(sz), sentTime(std::chrono::steady_clock::now()) {}
    };
    
    struct PacketGroup {
        std::vector<PacketInfo> packets;
        std::chrono::steady_clock::time_point departureTime;
        std::chrono::microseconds arrivalDelta;
    };
    
    TwccPacketTracker();
    ~TwccPacketTracker() = default;
    
    // Record a packet being sent
    void recordPacket(uint16_t twccSeqNum, uint16_t size);
    
    // Process TWCC feedback from client
    void processFeedback(uint16_t baseSeqNum, uint16_t packetCount,
                         const std::vector<bool>& received,
                         const std::vector<std::chrono::microseconds>& deltas);
    
    // Get packet groups for bandwidth estimation
    std::vector<PacketGroup> getPacketGroups(size_t maxGroups = 10) const;
    
    // Clean up old packets
    void cleanup(std::chrono::milliseconds maxAge = std::chrono::milliseconds(10000));
    
    // Get statistics
    struct Stats {
        uint32_t totalPackets = 0;
        uint32_t receivedPackets = 0;
        uint32_t lostPackets = 0;
        double lossRate = 0.0;
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
    };
    Stats getStats() const;
    
private:
    mutable std::mutex mMutex;
    std::map<uint16_t, PacketInfo> mPackets; // Keyed by TWCC sequence number
    std::chrono::steady_clock::time_point mLastCleanup;
    uint16_t mHighestFeedbackSeq = 0;  // Highest sequence number we've received feedback for
    bool mHasReceivedFeedback = false; // Whether we've received any feedback yet

    // Helper to handle sequence number wraparound
    static bool isNewerSeqNum(uint16_t a, uint16_t b);
};

} // namespace rtc::impl

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_IMPL_TWCC_PACKET_TRACKER_H */
