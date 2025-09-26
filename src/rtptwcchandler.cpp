/**
 * Copyright (c) 2025 Jacob Sologub
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "rtptwcchandler.hpp"
#include "impl/twccpackettracker.hpp"
#include "impl/twccfeedbackparser.hpp"
#include "impl/internals.hpp"
#include "rtp.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <cstring>
#include <memory>

namespace rtc {

RtpTwccHandler::RtpTwccHandler(uint8_t extId)
    : MediaHandler(), 
      mTwccExtId(extId),
      mPacketTracker(std::make_shared<impl::TwccPacketTracker>()) {
    PLOG_DEBUG << "RtpTwccHandler: Created with extension ID " << static_cast<int>(extId);
}

RtpTwccHandler::~RtpTwccHandler() = default;

TwccStats RtpTwccHandler::getStats() const {
    TwccStats result;
    if (mPacketTracker) {
        auto stats = mPacketTracker->getStats();
        result.packetsSent = stats.totalPackets;
        result.packetsReceived = stats.receivedPackets;
        result.packetsLost = stats.lostPackets;
        result.lossRate = stats.lossRate;
        result.bytesSent = stats.bytesSent;
        result.bytesReceived = stats.bytesReceived;
    }
    return result;
}

void RtpTwccHandler::processFeedback(const RtcpTwcc* twcc) {
    if (!twcc || !mPacketTracker) return;
    
    // Parse the feedback using the static method
    // Calculate total size from RTCP header
    size_t totalSize = twcc->header.header.lengthInBytes();
    auto parsed = impl::TwccFeedbackParser::parse(twcc, totalSize);
    
    if (!parsed) {
        PLOG_WARNING << "Failed to parse TWCC feedback";
        return;
    }
    
    // Build received and deltas vectors for the packet tracker
    std::vector<bool> received;
    std::vector<std::chrono::microseconds> deltas;
    
    for (const auto& packet : parsed->packets) {
        bool isReceived = (packet.status != impl::TwccFeedbackParser::PacketStatus::NotReceived);
        received.push_back(isReceived);
        // Add delta for all packets (0 for not received)
        deltas.push_back(isReceived ? packet.receiveDelta : std::chrono::microseconds(0));
    }
    
    // Process feedback in the tracker
    mPacketTracker->processFeedback(parsed->baseSeqNum,
									static_cast<uint16_t>(parsed->packets.size()),
                                    received, deltas);
}

void RtpTwccHandler::outgoing(message_vector &messages, const message_callback &send) {
    // Process each outgoing RTP packet to add TWCC sequence numbers
    for (auto &message : messages) {
        // Only process binary RTP packets with valid headers
        if (message->type == Message::Binary && message->size() >= sizeof(RtpHeader)) {
            auto rtpHeader = reinterpret_cast<RtpHeader*>(message->data());
            
            // Check if packet already has an RTP extension header
            auto extHeader = rtpHeader->getExtensionHeader();
            
            if (!extHeader) {
                // No extension header exists - we need to create one
                // This involves:
                // 1. Creating a new larger buffer
                // 2. Copying RTP header to new buffer
                // 3. Adding extension header
                // 4. Copying payload to new position (after extension)
                
                // Calculate sizes for buffer reallocation
                size_t currentHeaderSize = rtpHeader->getSize(); // Fixed header + CSRC list
                size_t payloadSize = message->size() - currentHeaderSize;
                
                // Extension header structure:
                // - 4 bytes: extension header (profile ID + length)
                // - 4 bytes: extension data (TWCC + padding)
                // Total: 8 bytes added to packet
                size_t extHeaderSize = sizeof(RtpExtensionHeader) + 4; // 4 bytes data, padded to 32-bit
                
                // Allocate new buffer with room for the extension
                auto newMessage = std::make_shared<Message>(message->size() + extHeaderSize);
                newMessage->type = message->type;
                
                // Step 1: Copy original RTP header (12 bytes + CSRC list)
                std::memcpy(newMessage->data(), message->data(), currentHeaderSize);
                
                // Step 2: Update RTP header to indicate extension present
                auto newRtpHeader = reinterpret_cast<RtpHeader*>(newMessage->data());
                newRtpHeader->setExtension(true); // Set X bit in RTP header
                
                // Step 3: Configure the extension header (located after RTP header)
                extHeader = newRtpHeader->getExtensionHeader();
                extHeader->setProfileSpecificId(0xBEDE); // One-byte header format
                extHeader->setHeaderLength(1); // Length in 32-bit words minus 1 (so 1 = 4 bytes)
                
                // Step 4: Initialize extension data area to zeros
                extHeader->clearBody();
                
                // Step 5: Copy RTP payload to its new position (after the extension)
                if (payloadSize > 0) {
                    // Source: original buffer + original header size
                    // Destination: new buffer + original header size + extension size
                    std::memcpy(newMessage->data() + currentHeaderSize + extHeaderSize,
                                message->data() + currentHeaderSize,
                                payloadSize);
                }
                
                // Replace the message with our new extended version
                message = newMessage;
                rtpHeader = newRtpHeader;
            }
            
            // Now add TWCC sequence number to the extension header
            if (extHeader) {
                // Always allocate a sequence number atomically to avoid duplicates
                // Even if we fail to add the extension, we must track this as a gap
                uint16_t seqNum = mTwccSeqNum.fetch_add(1);

                // Get pointer to extension data area and calculate available space
                auto extData = reinterpret_cast<uint8_t*>(extHeader->getBody());
                // headerLength() returns length in 32-bit words, so multiply by 4 for bytes
                size_t extDataSize = extHeader->headerLength() * 4;
                bool added = false;
                
                // Check if we have any space at all
                if (extDataSize == 0) {
                    // This shouldn't happen if extension was properly created
                    PLOG_WARNING << "TWCC: Extension header has no data area (size=0)";
                } else if (extHeader->profileSpecificId() == 0xBEDE) {
                    // === ONE-BYTE HEADER FORMAT (RFC 5285) ===
                    // Extension format: [ID:4 bits][Length-1:4 bits][Data:N bytes]
                    // - ID: Extension identifier (1-14, 0=padding, 15=reserved)
                    // - Length-1: Data length minus 1 (0 means 1 byte, max 15 means 16 bytes)
                    // - Data: Extension-specific data
                    
                    size_t offset = 0;
                    // Scan through existing extensions to find where to place TWCC
                    while (offset < extDataSize) {
                        // Extract ID from upper 4 bits
                        uint8_t id = (extData[offset] >> 4) & 0x0F;
                        // Extract length from lower 4 bits and add 1 for actual length
                        uint8_t len = (extData[offset] & 0x0F) + 1;
                        
                        if (id == 0) {
                            // ID=0 means padding byte - we can overwrite with TWCC
                            // Need 3 bytes: 1 for header + 2 for sequence number
                            if (offset + 3 <= extDataSize) {
                                // Write TWCC extension header: [ID:4][Len-1:4]
                                extData[offset] = (mTwccExtId << 4) | 0x01; // Length=2, so Len-1=1
                                // Write 16-bit sequence number in network byte order (big-endian)
                                extData[offset + 1] = (seqNum >> 8) & 0xFF; // High byte
                                extData[offset + 2] = seqNum & 0xFF;        // Low byte
                                added = true;
                                break;
                            }
                        } else if (id == mTwccExtId) {
                            // TWCC extension already exists - just update sequence number
                            // Verify we have space for the data (should always be true if well-formed)
                            if (offset + 1 + len <= extDataSize && len >= 2) {
                                extData[offset + 1] = (seqNum >> 8) & 0xFF;
                                extData[offset + 2] = seqNum & 0xFF;
                                added = true;
                            }
                            break;
                        }
                        
                        // Move to next extension entry
                        offset += 1 + len; // 1 byte header + data length
                    }
                    
                    // If we scanned all extensions and found no slot, try appending
                    if (!added && offset + 3 <= extDataSize) {
                        // Append TWCC at the end of existing extensions
                        extData[offset] = (mTwccExtId << 4) | 0x01;
                        extData[offset + 1] = (seqNum >> 8) & 0xFF;
                        extData[offset + 2] = seqNum & 0xFF;
                        added = true;
                    }
                    
                } else if (extHeader->profileSpecificId() == 0x1000) {
                    // === TWO-BYTE HEADER FORMAT (RFC 5285) ===
                    // Extension format: [ID:8 bits][Length:8 bits][Data:N bytes]
                    // - ID: Extension identifier (0=padding, 1-255=valid)
                    // - Length: Data length in bytes (0-255)
                    // - Data: Extension-specific data
                    
                    size_t offset = 0;
                    // Scan through existing extensions
                    while (offset + 1 < extDataSize) {
                        uint8_t id = extData[offset];      // Full byte for ID
                        uint8_t len = extData[offset + 1]; // Full byte for length
                        
                        if (id == 0) {
                            // ID=0 means padding - we can use this space
                            // Need 4 bytes: 2 for header + 2 for sequence number
                            if (offset + 4 <= extDataSize) {
                                extData[offset] = mTwccExtId;       // Extension ID
                                extData[offset + 1] = 2;            // Length = 2 bytes
                                extData[offset + 2] = (seqNum >> 8) & 0xFF; // Seq high byte
                                extData[offset + 3] = seqNum & 0xFF;        // Seq low byte
                                added = true;
                                break;
                            }
                        } else if (id == mTwccExtId) {
                            // TWCC already exists - update sequence number
                            // Verify we have space for the data
                            if (offset + 2 + len <= extDataSize && len >= 2) {
                                extData[offset + 2] = (seqNum >> 8) & 0xFF;
                                extData[offset + 3] = seqNum & 0xFF;
                                added = true;
                            }
                            break;
                        }
                        
                        // Move to next extension entry
                        offset += 2 + len; // 2 bytes header + data length
                    }
                    
                    // Try appending at the end if no slot found
                    if (!added && offset + 4 <= extDataSize) {
                        // Append TWCC at the end of existing extensions
                        extData[offset] = mTwccExtId;
                        extData[offset + 1] = 2;
                        extData[offset + 2] = (seqNum >> 8) & 0xFF;
                        extData[offset + 3] = seqNum & 0xFF;
                        added = true;
                    }
                }
                
                if (added) {
                    // Successfully added TWCC - track this packet for bandwidth estimation
                    if (mPacketTracker) {
                        mPacketTracker->recordPacket(seqNum, message->size());
                    }

                    // Also track in our local map for getSentPacketInfo()
                    {
                        std::lock_guard<std::mutex> lock(mSentPacketsMutex);
                        SentPacketInfo info;
                        info.sendTime = std::chrono::steady_clock::now();
                        info.size = message->size();
                        mSentPackets[seqNum] = info;

                        // Clean up old entries if map gets too large
                        if (mSentPackets.size() > 1000) {
                            // Simple cleanup: remove entries older than 10 seconds
                            auto now = std::chrono::steady_clock::now();
                            auto it = mSentPackets.begin();
                            while (it != mSentPackets.end()) {
                                if (now - it->second.sendTime > std::chrono::seconds(10)) {
                                    it = mSentPackets.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                        }
                    }
                } else {
                    // Failed to add extension but sequence number was already allocated
                    // Track this as a gap (size=0) so we handle feedback correctly
                    if (mPacketTracker) {
                        mPacketTracker->recordPacket(seqNum, 0); // Size 0 indicates a gap
                    }
                    PLOG_WARNING << "TWCC: Failed to add seq=" << seqNum
                                 << " extension - tracked as gap for feedback processing";
                }
            }
        }
    }
    
    // Forward all messages to the next handler in the chain
    // Note: Messages in the vector may have been replaced with new buffers (when adding
    // extension headers) or modified in-place (when updating existing extensions)
    for (auto &message : messages) {
        send(message);
    }
}

std::optional<RtpTwccHandler::ParsedFeedback>
RtpTwccHandler::parseTwccFeedback(const RtcpTwcc* twcc, size_t totalSize) {
    if (!twcc) return std::nullopt;

    // Use the internal parser
    auto parsed = impl::TwccFeedbackParser::parse(twcc, totalSize);
    if (!parsed) return std::nullopt;

    // Convert to public API format
    ParsedFeedback result;
    result.baseSeqNum = parsed->baseSeqNum;
    result.referenceTimeMs = parsed->referenceTime * 64; // Convert from 64ms units to ms

    for (const auto& packet : parsed->packets) {
        PacketInfo info;
        info.seqNum = packet.seqNum;
        info.received = (packet.status != impl::TwccFeedbackParser::PacketStatus::NotReceived);
        info.receiveDelta = packet.receiveDelta;
        result.packets.push_back(info);
    }

    return result;
}

std::optional<RtpTwccHandler::SentPacketInfo>
RtpTwccHandler::getSentPacketInfo(uint16_t seqNum) const {
    std::lock_guard<std::mutex> lock(mSentPacketsMutex);
    auto it = mSentPackets.find(seqNum);
    if (it != mSentPackets.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

