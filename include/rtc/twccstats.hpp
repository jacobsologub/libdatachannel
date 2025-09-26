/**
 * Copyright (c) 2025 Jacob Sologub
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_TWCC_STATS_H
#define RTC_TWCC_STATS_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"

namespace rtc {

// Public stats structure for TWCC packet tracking
struct RTC_CPP_EXPORT TwccStats {
    uint32_t packetsSent = 0;
    uint32_t packetsReceived = 0;
    uint32_t packetsLost = 0;
    double lossRate = 0.0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_TWCC_STATS_H */