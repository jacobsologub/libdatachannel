/**
 * Copyright (c) 2025 Jacob Sologub
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "twccpackettracker.hpp"
#include "internals.hpp"

#include <algorithm>
#include <sstream>

namespace rtc::impl {

TwccPacketTracker::TwccPacketTracker()
    : mLastCleanup(std::chrono::steady_clock::now()) {
}

void TwccPacketTracker::recordPacket(uint16_t twccSeqNum, uint16_t size) {
    std::lock_guard lock(mMutex);
    
    // Auto-cleanup old packets periodically
    auto now = std::chrono::steady_clock::now();
    if (now - mLastCleanup > std::chrono::seconds(5)) {
        cleanup();
        mLastCleanup = now;
    }
    
    mPackets.emplace(twccSeqNum, PacketInfo(twccSeqNum, size));
}

void TwccPacketTracker::processFeedback(uint16_t baseSeqNum, uint16_t packetCount,
                                        const std::vector<bool>& received,
                                        const std::vector<std::chrono::microseconds>& deltas) {
    std::lock_guard lock(mMutex);

    if (received.size() != packetCount || deltas.size() != packetCount) {
        PLOG_WARNING << "TWCC: Feedback size mismatch";
        return;
    }

    // Clean up old packets periodically (keep last 10 seconds for better tracking)
    cleanup(std::chrono::milliseconds(10000));

    uint16_t seqNum = baseSeqNum;
    uint32_t receivedCount = 0;
    uint32_t notFoundCount = 0;
    uint32_t foundCount = 0;

    // Track the highest sequence number we've received feedback for
    uint16_t feedbackEndSeq = uint16_t(baseSeqNum + packetCount - 1);
    if (!mHasReceivedFeedback || isNewerSeqNum(feedbackEndSeq, mHighestFeedbackSeq)) {
        mHighestFeedbackSeq = feedbackEndSeq;
        mHasReceivedFeedback = true;
    }

    for (size_t i = 0; i < packetCount; ++i) {
        auto it = mPackets.find(seqNum);
        if (it != mPackets.end()) {
            foundCount++;
            it->second.received = received[i];
            if (received[i]) {
                it->second.receiveDelta = deltas[i];
                receivedCount++;
            }
        } else {
            // Packet not found in our tracking
            notFoundCount++;
            if (notFoundCount <= 5) { // Log first few missing packets
                PLOG_WARNING << "TWCC: Packet seq=" << seqNum
                             << " (offset " << i << " from base " << baseSeqNum
                             << ") in feedback but not tracked"
                             << " (received=" << (received[i] ? "YES" : "NO") << ")";
            }
        }
        seqNum++;
    }

    if (notFoundCount > 0) {
        PLOG_WARNING << "TWCC: " << notFoundCount << "/" << packetCount
                     << " packets in feedback were not tracked"
                     << " (found=" << foundCount
                     << " received=" << receivedCount << ")";

        // This is likely because we're missing packets in our tracking
        // The client is reporting on a continuous range but we may have gaps
        // This could happen if:
        // 1. Some packets didn't get TWCC extensions added
        // 2. Some non-RTP packets were in the stream
        // 3. Packets were cleaned up too early (unlikely with 10s window)
        // 4. Sequence number mismatch between what we sent and what's reported
    }
}

std::vector<TwccPacketTracker::PacketGroup> TwccPacketTracker::getPacketGroups(size_t maxGroups) const {
    std::lock_guard lock(mMutex);
    
    std::vector<PacketGroup> groups;
    if (mPackets.empty()) {
        return groups;
    }
    
    // Group packets by similar send times (within 5ms)
    const auto groupingThreshold = std::chrono::milliseconds(5);
    
    PacketGroup currentGroup;
    for (const auto& [seq, packet] : mPackets) {
        if (!packet.received || !packet.receiveDelta.has_value()) {
            continue;
        }
        
        if (currentGroup.packets.empty()) {
            currentGroup.packets.push_back(packet);
            currentGroup.departureTime = packet.sentTime;
        } else {
            auto timeDiff = packet.sentTime - currentGroup.departureTime;
            if (timeDiff <= groupingThreshold) {
                currentGroup.packets.push_back(packet);
            } else {
                // Start new group
                if (!currentGroup.packets.empty()) {
                    groups.push_back(currentGroup);
                    if (groups.size() >= maxGroups) {
                        break;
                    }
                }
                currentGroup = PacketGroup();
                currentGroup.packets.push_back(packet);
                currentGroup.departureTime = packet.sentTime;
            }
        }
    }
    
    if (!currentGroup.packets.empty() && groups.size() < maxGroups) {
        groups.push_back(currentGroup);
    }
    
    return groups;
}

void TwccPacketTracker::cleanup(std::chrono::milliseconds maxAge) {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - maxAge;
    
    auto it = mPackets.begin();
    while (it != mPackets.end()) {
        if (it->second.sentTime < cutoff) {
            it = mPackets.erase(it);
        } else {
            ++it;
        }
    }
}

TwccPacketTracker::Stats TwccPacketTracker::getStats() const {
    std::lock_guard lock(mMutex);

    Stats stats;

    // Extended window to ensure we capture feedback that arrives late
    auto now = std::chrono::steady_clock::now();
    auto statsWindow = now - std::chrono::seconds(5); // Increased from 2s to 5s

    // Google WebRTC uses 1 second for feedback timeout in normal conditions
    // We'll use 1.5 seconds to be conservative for varying network conditions
    auto feedbackTimeout = now - std::chrono::milliseconds(1500);

    uint32_t pendingPackets = 0;

    for (const auto& [seq, packet] : mPackets) {
        if (packet.sentTime >= statsWindow) {
            // Skip gap packets (size=0) from statistics
            // These are sequence numbers we allocated but couldn't add extensions to
            if (packet.size == 0) {
                continue;
            }

            stats.bytesSent += packet.size;

            if (packet.received) {
                // Packet was acknowledged as received
                stats.receivedPackets++;
                stats.bytesReceived += packet.size;
            } else if (packet.sentTime < feedbackTimeout) {
                // Packet is old enough that feedback should have arrived
                // But only count as lost if it's within the feedback window
                if (mHasReceivedFeedback && !isNewerSeqNum(seq, mHighestFeedbackSeq)) {
                    // This packet is within the feedback window
                    // If we haven't received confirmation, it's lost
                    stats.lostPackets++;
                }
            } else {
                // Packet is too recent - still waiting for feedback
                pendingPackets++;
            }
        }
    }

    // Only count packets that have had sufficient time for feedback
    stats.totalPackets = stats.receivedPackets + stats.lostPackets;

    // Calculate loss rate only from packets with determined status
    if (stats.totalPackets > 0) {
        stats.lossRate = static_cast<double>(stats.lostPackets) / stats.totalPackets;
    } else {
        // No packets have had enough time for feedback yet
        stats.lossRate = 0.0;
    }

    // Sanity check - log if we see unrealistic loss rates on what should be a good connection
    if (stats.lossRate > 0.5 && stats.totalPackets > 10) {
        PLOG_WARNING << "High loss rate detected: " << (stats.lossRate * 100) << "% "
                     << "(lost=" << stats.lostPackets
                     << " received=" << stats.receivedPackets
                     << " pending=" << pendingPackets << ")";
    }

    return stats;
}

bool TwccPacketTracker::isNewerSeqNum(uint16_t a, uint16_t b) {
    // Handle 16-bit sequence number wraparound
    // Consider a "newer" than b if the distance from b to a is less than half the range
    const uint16_t halfRange = 0x8000;
    return ((a - b) & 0xFFFF) < halfRange;
}

} // namespace rtc::impl

#endif /* RTC_ENABLE_MEDIA */
