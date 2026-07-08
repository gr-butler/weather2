#pragma once

#include <cstdint>

#include "buffer.h"
#include "constants.h"

// Pure rain calculations — ported from weather/sensors/Rainmeter.go.
// Arduino-independent so they can be unit tested on the host. The interrupt,
// software debounce and per-minute bookkeeping live in rainmeter.cpp; this
// header only performs the maths so both the firmware and the host tests share
// a single source of truth.

// GetRate(): millimetres of rain over the last hour.
//   rate = MmPerTip * sum(tipBuf)
// tipBuf holds one slot per minute for the last hour (RainBufferMinutes slots),
// each slot being the number of bucket tips counted in that minute.
//
// NOTE (inherited from the Go reference SampleBuffer): the FIRST value pushed
// into tipBuf pre-fills every slot with that value. So a single tip counted in
// the very first minute of operation is replicated across all 60 slots and
// briefly inflates the reported rate by a factor of RainBufferMinutes until the
// window flushes. Calm start-up (first minute == 0 tips) pre-fills zeros and
// reports 0, which is the normal case.
inline double rainRateMmPerHour(SampleBuffer &tipBuf) {
    BufferStats s = tipBuf.getAverageMinMaxSum();
    return static_cast<double>(MmPerTip) * s.sum;
}

// Convert a raw bucket-tip count to millimetres. Used for the day total
// (GetDayAccumulation) and the since-last-report total (GetAccumulation).
//   mm = MmPerTip * tips
inline double rainMmFromTips(uint32_t tips) {
    return static_cast<double>(MmPerTip) * static_cast<double>(tips);
}
