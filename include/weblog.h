#pragma once

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// In-memory tee for serial diagnostics. Everything printed through the global
// `Log` object is written to the real UART (unchanged behaviour) AND kept in a
// small ring buffer so the web UI can show a live, auto-updating log at /logs.
//
// The .cpp source files alias `Serial` to this object (see the `#define Serial
// Log` after their includes), so existing Serial.print/println/printf calls are
// captured with no other changes. This header and weblog.cpp deliberately do
// NOT alias Serial, so they still talk to the real hardware UART.
class WebLog : public Print {
public:
    static constexpr size_t kMaxLines = 200;   // ring buffer depth
    static constexpr size_t kMaxLineLen = 140; // per-line cap (incl. NUL)

    WebLog();

    // Forwarders so aliased `Serial.begin()` / `Serial.flush()` keep working.
    void begin(unsigned long baud = 115200);
    void flush();

    // Print interface — tees each byte to the UART and the ring buffer.
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;

    // Sequence number of the most recently completed line (0 if none yet).
    uint32_t latestSeq();

    // Build a JSON document of the buffered lines with seq > afterSeq, oldest
    // first:
    //   {"seq":<latest>,"lines":[{"n":<seq>,"t":"<text>"},...]}
    // Pass afterSeq = 0 to start from the oldest buffered line. maxLines caps
    // how many lines one call returns (0 = no cap), so a caller can page
    // through the buffer in small chunks rather than one large allocation:
    // continue from the "n" of the last line returned until it reaches "seq".
    String jsonSince(uint32_t afterSeq, size_t maxLines = 0);

private:
    struct Line {
        uint32_t seq;
        char text[kMaxLineLen];
    };

    void pushLineLocked();

    Line lines_[kMaxLines];
    size_t head_ = 0;  // index of the next slot to fill
    size_t count_ = 0; // number of valid lines held
    uint32_t seq_ = 0; // last assigned line sequence number

    char cur_[kMaxLineLen]; // line currently being assembled
    size_t curLen_ = 0;

    SemaphoreHandle_t mtx_;
};

extern WebLog Log;
