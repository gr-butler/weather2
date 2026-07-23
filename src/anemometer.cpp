#include "anemometer.h"

#include "driver/twai.h"

#include "can_protocol.h"
#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// CAN/TWAI wind-data ingestion. See anemometer.h for the OPTION B sampling
// model. Calculations live in wind_math.h (ported from Anemometer.go).

namespace {
constexpr const char *kWindCfgNamespace = "windcfg";
constexpr const char *kKeyDirAvgSec = "dirAvgSec";
constexpr const char *kKeyAppSumSec = "appSumSec";
constexpr const char *kKeyDirBufSec = "dirBufSec";
// Maximum direction buffer: 300 s × 4 Hz = 1200 samples ≈ 10 KB of doubles.
// Speed and gust buffers are fixed at WindBufferSamples (240) and are not
// resized — their size is mandated by the rolling-average maths (AGENTS.md Rule 2).
constexpr int kDirBufMaxSeconds = 300;
} // namespace (windcfg keys)

namespace {
constexpr unsigned long WindSampleIntervalMs = 1000 / WindSamplesPerSecond; // 250 ms
// A CAN frame is expected every 250 ms. If none has been *decoded* within
// FrameStaleMs the current slot is filled with 0 pulses; after FrameOfflineMs of
// continuous silence the sensor is marked offline. These are wall-clock windows
// (not per-slot counters) so a burst catch-up after the main loop was blocked
// by a slow network call does not read as "missed frames".
constexpr unsigned long FrameStaleMs = 500;     // ~2 missed 250 ms slots
constexpr unsigned long FrameOfflineMs = 1250;  // ~5 missed 250 ms slots
// Depth of the TWAI receive FIFO. The default is only 5 frames (~1.25 s at
// 4 Hz); a slow blocking call in loop() (river HTTPS fetch, MQTT/WOW over TLS)
// can starve the drain long enough to overflow it and drop frames. 32 frames
// buys ~8 s of headroom.
constexpr uint32_t RxQueueLen = 32;
// Dedicated RX task. Pinned to core 0 (the Arduino loop runs on core 1), so it
// keeps draining the FIFO and ticking the 250 ms sampler even while loop() is
// blocked in a slow network call. It spends almost all its time asleep inside
// twai_receive(), so CPU cost is negligible.
constexpr uint32_t CanTaskStackWords = 4096;
constexpr UBaseType_t CanTaskPriority = 5; // above loop() (1), below WiFi
constexpr BaseType_t CanTaskCore = 0;
constexpr unsigned long RxBlockMs = 20; // max sleep between sampler ticks
} // namespace

void Anemometer::loadWindConfig() {
    windPrefs_.begin(kWindCfgNamespace, /*readOnly=*/true);
    dirAvgSeconds_     = windPrefs_.getInt(kKeyDirAvgSec, WindDirectionAverageSeconds);
    appSummarySeconds_ = windPrefs_.getInt(kKeyAppSumSec, AppWindSummarySeconds);
    dirBufSeconds_     = windPrefs_.getInt(kKeyDirBufSec, WindBufferLengthSeconds);
    windPrefs_.end();
    // Clamp to valid range in case NVS holds a stale/out-of-range value.
    dirBufSeconds_     = max(1, min(dirBufSeconds_,     kDirBufMaxSeconds));
    dirAvgSeconds_     = max(1, min(dirAvgSeconds_,     dirBufSeconds_));
    appSummarySeconds_ = max(1, min(appSummarySeconds_, WindBufferLengthSeconds));
    // Resize the direction buffer if it was persisted larger than the default.
    if (dirBufSeconds_ != WindBufferLengthSeconds) {
        dirBuf_.resize(dirBufSeconds_ * WindSamplesPerSecond);
    }
    Serial.printf("Wind config: dir_avg=%ds dir_buf=%ds app_window=%ds\n",
                  dirAvgSeconds_, dirBufSeconds_, appSummarySeconds_);
}

void Anemometer::saveWindConfig() {
    windPrefs_.begin(kWindCfgNamespace, /*readOnly=*/false);
    windPrefs_.putInt(kKeyDirAvgSec, dirAvgSeconds_);
    windPrefs_.putInt(kKeyAppSumSec, appSummarySeconds_);
    windPrefs_.putInt(kKeyDirBufSec, dirBufSeconds_);
    windPrefs_.end();
}

void Anemometer::setDirAvgSeconds(int s) {
    s = max(1, min(s, kDirBufMaxSeconds));
    // Auto-grow the direction buffer if the requested window exceeds it.
    if (s > dirBufSeconds_) {
        if (mutex_ != nullptr) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
        }
        dirBuf_.resize(s * WindSamplesPerSecond);
        dirBufSeconds_ = s;
        if (mutex_ != nullptr) {
            xSemaphoreGive(mutex_);
        }
        Serial.printf("Wind dir_buf auto-grown to %ds (%d samples)\n",
                      s, s * WindSamplesPerSecond);
    }
    dirAvgSeconds_ = s;
    saveWindConfig();
    Serial.printf("Wind dir_avg set to %ds\n", s);
}

void Anemometer::setAppSummarySeconds(int s) {
    s = max(1, min(s, WindBufferLengthSeconds));
    appSummarySeconds_ = s;
    saveWindConfig();
    Serial.printf("Wind app_window set to %ds\n", s);
}

void Anemometer::setDirBufSeconds(int s) {
    s = max(1, min(s, kDirBufMaxSeconds));
    // If shrinking below the current averaging window, pull dir-avg down too.
    if (s < dirAvgSeconds_) {
        dirAvgSeconds_ = s;
        Serial.printf("Wind dir_avg reduced to %ds to fit new buffer\n", s);
    }
    // Resize while holding the anemometer mutex so sampleSlot() (core 0)
    // doesn't write into the buffer during the operation.
    if (mutex_ != nullptr) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    dirBuf_.resize(s * WindSamplesPerSecond);
    dirBufSeconds_ = s;
    if (mutex_ != nullptr) {
        xSemaphoreGive(mutex_);
    }
    saveWindConfig();
    Serial.printf("Wind dir_buf set to %ds (%d samples, buffer reset)\n",
                  s, s * WindSamplesPerSecond);
}

bool Anemometer::begin() {
    loadWindConfig();
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        static_cast<gpio_num_t>(TX_GPIO_NUM),
        static_cast<gpio_num_t>(RX_GPIO_NUM), TWAI_MODE_NORMAL);
    g_config.rx_queue_len = RxQueueLen; // deeper FIFO to survive loop() stalls

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        Serial.println("Failed to install TWAI driver");
        return false;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("Failed to start TWAI driver");
        twai_driver_uninstall();
        return false;
    }
    driverInstalled_ = true;
    lastSampleMs_ = millis();
    Serial.printf("Wind CAN driver online (TX=%d RX=%d @ %lu bps)\n",
                  (int)TX_GPIO_NUM, (int)RX_GPIO_NUM,
                  (unsigned long)WIND_CAN_BITRATE);

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        Serial.println("Failed to create wind mutex");
        twai_stop();
        twai_driver_uninstall();
        driverInstalled_ = false;
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(&Anemometer::canRxTask, "canRx",
                                            CanTaskStackWords, this,
                                            CanTaskPriority, &task_, CanTaskCore);
    if (ok != pdPASS) {
        Serial.println("Failed to start wind CAN RX task");
        twai_stop();
        twai_driver_uninstall();
        driverInstalled_ = false;
        return false;
    }
    Serial.printf("Wind CAN RX task started on core %d\n", (int)CanTaskCore);
    return true;
}

void Anemometer::canRxTask(void *arg) {
    static_cast<Anemometer *>(arg)->taskLoop();
}

void Anemometer::taskLoop() {
    lastSampleMs_ = millis();
    for (;;) {
        // Block (briefly) for the next frame, then drain any others already
        // queued. Blocking here means the task sleeps when the bus is idle.
        twai_message_t msg;
        if (twai_receive(&msg, pdMS_TO_TICKS(RxBlockMs)) == ESP_OK) {
            do {
                if (msg.identifier == WIND_CAN_ID && !msg.rtr) {
                    processFrame(msg);
                }
            } while (twai_receive(&msg, 0) == ESP_OK);
        }

        // Advance the 250 ms sampling timer, catching up any elapsed slots so
        // the buffers are always filled at exactly 4 Hz.
        unsigned long now = millis();
        while (now - lastSampleMs_ >= WindSampleIntervalMs) {
            lastSampleMs_ += WindSampleIntervalMs;
            sampleSlot();
        }
    }
}

void Anemometer::processFrame(const twai_message_t &msg) {
    WindFrame frame;
    if (!decodeWindFrame(msg.data, msg.data_length_code, frame)) {
        return;
    }
    // Track the frame counter to tell lost frames apart from a masthead
    // reboot. The counter resets to 0 on the masthead's boot and otherwise
    // increments by 1 per frame.
    if (frame.hasSeq) {
        if (haveSeq_) {
            if (frame.seq == lastSeq_ + 1) {
                // Contiguous — nothing lost.
            } else if (frame.seq > lastSeq_) {
                Serial.printf("Wind: %lu CAN frame(s) lost (seq %lu -> %lu)\n",
                              (unsigned long)(frame.seq - lastSeq_ - 1),
                              (unsigned long)lastSeq_,
                              (unsigned long)frame.seq);
            } else {
                Serial.printf("Wind: masthead reboot detected (seq %lu -> %lu)\n",
                              (unsigned long)lastSeq_,
                              (unsigned long)frame.seq);
            }
        }
        lastSeq_ = frame.seq;
        haveSeq_ = true;
    }
    // Per-frame pulse sanity clamp (mirrors Go pulseCount > 25 -> 0).
    uint8_t pulses = frame.pulseCount;
    if (pulses > WIND_PULSE_MAX) {
        Serial.printf("Pulse count error [%u]\n", (unsigned)pulses);
        pulses = 0;
    }
    accumPulses_ += pulses;
    frameThisWindow_ = true;
    lastFrameMs_ = millis();
    haveFrameEver_ = true;
    // Keep the most recent direction reading for this window.
    pendingDirection_ = windVoltToDegrees(windAdcToVolts(frame.adcRaw), &dirStr_);
    haveDirection_ = true;
}

void Anemometer::logCanStatus(unsigned long now) {
    if (now - lastStatusLogMs_ < 30000) {
        return;
    }
    lastStatusLogMs_ = now;

    if (!haveFrameEver_) {
        Serial.println("Wind: CAN status state=waiting data=no frames=0 age_ms=n/a");
        return;
    }

    const unsigned long ageMs = now - lastFrameMs_;
    const bool hasRecentData = ageMs <= FrameStaleMs;
    const char *state = ageMs >= FrameOfflineMs ? "offline" : "online";
    Serial.printf("Wind: CAN status state=%s data=%s age_ms=%lu\n", state,
                  hasRecentData ? "yes" : "no", ageMs);
}

void Anemometer::sampleSlot() {
    // Insert exactly one sample for the elapsed 250 ms slot. The rolling buffers
    // are read by the getters on other cores, so hold the mutex while writing.
    double pulses = static_cast<double>(accumPulses_);
    if (mutex_ != nullptr) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    speedBuf_.addItem(pulses);
    gustBuf_.addItem(pulses);
    // Direction: only trust it when the wind is actually turning the cups,
    // otherwise hold the last valid bearing (matches the Go zero-wind rule).
    if (accumPulses_ > 0 && haveDirection_) {
        lastDirection_ = pendingDirection_;
        dirBuf_.addItem(pendingDirection_);
    } else {
        dirBuf_.addItem(dirBuf_.getLast());
    }
    if (mutex_ != nullptr) {
        xSemaphoreGive(mutex_);
    }

    unsigned long now = millis();
    // Online/offline is judged on how long it has been since the last frame was
    // actually decoded, not on this single slot. This stays correct when the
    // loop was blocked and several slots are caught up in one burst.
    if (!haveFrameEver_) {
        // Nothing heard from the masthead yet; stay offline silently.
    } else if (now - lastFrameMs_ <= FrameStaleMs) {
        online_ = true;
    } else {
        if (now - lastMissedLogMs_ >= 5000) {
            lastMissedLogMs_ = now;
            Serial.printf("Wind: no CAN frame for %lu ms, inserting 0\n",
                          (unsigned long)(now - lastFrameMs_));
        }
        if (now - lastFrameMs_ >= FrameOfflineMs) {
            if (online_) {
                Serial.println("Wind sensor OFFLINE (no CAN frames)");
            }
            online_ = false;
        }
    }

    logCanStatus(now);

    // Reset the window accumulators.
    accumPulses_ = 0;
    frameThisWindow_ = false;
    haveDirection_ = false;
}
