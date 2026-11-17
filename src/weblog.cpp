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

String WebLog::jsonSince(uint32_t afterSeq, size_t maxLines) {
    String out;

    if (mtx_ == nullptr || xSemaphoreTake(mtx_, portMAX_DELAY) != pdTRUE) {
        out = "{\"seq\":0,\"lines\":[]}";
        return out;
    }

    // Size the output up front from the lines we will actually emit, so the
    // String does no incremental reallocation while building. Repeated
    // append-grows churn and fragment the heap in the async web task, a prime
    // suspect for the out-of-memory aborts seen there.
    size_t need = 24; // {"seq":<seq>,"lines":[]}
    {
        size_t sized = 0;
        size_t sizeIdx = (head_ + kMaxLines - count_) % kMaxLines;
        for (size_t i = 0; i < count_; i++) {
            const Line &line = lines_[sizeIdx];
            sizeIdx = (sizeIdx + 1) % kMaxLines;
            if (line.seq <= afterSeq) {
                continue;
            }
            if (maxLines != 0 && sized >= maxLines) {
                break;
            }
            sized++;
            // {"n":<seq>,"t":"<text>"}, plus slack for JSON escaping.
            need += strlen(line.text) + 28;
        }
    }
    out.reserve(need);

    out += "{\"seq\":";
    out += seq_;
    out += ",\"lines\":[";

    bool first = true;
    size_t emitted = 0;
    // Oldest valid line sits count_ slots behind head_.
    size_t idx = (head_ + kMaxLines - count_) % kMaxLines;
    for (size_t i = 0; i < count_; i++) {
        const Line &line = lines_[idx];
        idx = (idx + 1) % kMaxLines;
        if (line.seq <= afterSeq) {
            continue;
        }
        if (maxLines != 0 && emitted >= maxLines) {
            break;
        }
        emitted++;
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
