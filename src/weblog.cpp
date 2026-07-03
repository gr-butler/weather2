#include "weblog.h"

// Global singleton. Constructed before setup() runs; FreeRTOS is already up on
// the ESP32 at that point, so the mutex can be created in the constructor.
WebLog Log;

WebLog::WebLog() {
    mtx_ = xSemaphoreCreateMutex();
}

void WebLog::begin(unsigned long baud) {
    Serial.begin(baud);
}

void WebLog::flush() {
    Serial.flush();
}

void WebLog::pushLineLocked() {
    cur_[curLen_] = '\0';
    Line &line = lines_[head_];
    line.seq = ++seq_;
    memcpy(line.text, cur_, curLen_ + 1);
    head_ = (head_ + 1) % kMaxLines;
    if (count_ < kMaxLines) {
        count_++;
    }
    curLen_ = 0;
}

size_t WebLog::write(uint8_t c) {
    Serial.write(c); // tee to the real UART
    if (mtx_ != nullptr && xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) {
        if (c == '\n') {
            pushLineLocked();
        } else if (c != '\r' && curLen_ < kMaxLineLen - 1) {
            cur_[curLen_++] = static_cast<char>(c);
        }
        xSemaphoreGive(mtx_);
    }
    return 1;
}

size_t WebLog::write(const uint8_t *buffer, size_t size) {
    Serial.write(buffer, size); // tee to the real UART
    if (mtx_ != nullptr && xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < size; i++) {
            uint8_t c = buffer[i];
            if (c == '\n') {
                pushLineLocked();
            } else if (c != '\r' && curLen_ < kMaxLineLen - 1) {
                cur_[curLen_++] = static_cast<char>(c);
            }
        }
        xSemaphoreGive(mtx_);
    }
    return size;
}

uint32_t WebLog::latestSeq() {
    uint32_t s = 0;
    if (mtx_ != nullptr && xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) {
        s = seq_;
        xSemaphoreGive(mtx_);
    }
    return s;
}

String WebLog::jsonSince(uint32_t afterSeq) {
    String out;
    out.reserve(1024);
    out += "{\"seq\":";

    if (mtx_ == nullptr || xSemaphoreTake(mtx_, portMAX_DELAY) != pdTRUE) {
        out += "0,\"lines\":[]}";
        return out;
    }

    out += seq_;
    out += ",\"lines\":[";

    bool first = true;
    // Oldest valid line sits count_ slots behind head_.
    size_t idx = (head_ + kMaxLines - count_) % kMaxLines;
    for (size_t i = 0; i < count_; i++) {
        const Line &line = lines_[idx];
        idx = (idx + 1) % kMaxLines;
        if (line.seq <= afterSeq) {
            continue;
        }
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"n\":";
        out += line.seq;
        out += ",\"t\":\"";
        // Minimal JSON string escaping.
        for (const char *p = line.text; *p != '\0'; p++) {
            char ch = *p;
            switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out += ' '; // drop stray control chars
                } else {
                    out += ch;
                }
                break;
            }
        }
        out += "\"}";
    }
    xSemaphoreGive(mtx_);

    out += "]}";
    return out;
}
