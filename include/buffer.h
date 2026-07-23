#pragma once

#include <cstddef>
#include <limits>
#include <mutex>
#include <vector>

// SampleBuffer — fixed-size circular buffer.
// Ported from weather/buffer/buffer.go (Go reference implementation).
//
// Behaviour MUST match the reference, in particular:
//  * On the very first AddItem, every slot is pre-filled with that value
//    (so averages are taken over a full window from the start).
//  * GetAverageMinMaxSum divides the sum by the full buffer size, not by the
//    number of items inserted.

struct BufferStats {
    double average;
    double minimum;
    double maximum;
    double sum;
};

class SampleBuffer {
public:
    explicit SampleBuffer(int size)
        : position_(0), size_(size), first_(true), data_(size, 0.0) {}

    // Insert a value at the current position and advance (wrapping). On the
    // first insertion the whole buffer is filled with val.
    void addItem(double val) {
        std::lock_guard<std::mutex> guard(lock_);
        addItemNoLock(val);
    }

    // Add a value to the current (not-yet-advanced) slot.
    void addValueToCurrentItem(double val) {
        std::lock_guard<std::mutex> guard(lock_);
        data_[position_] += val;
    }

    // Average / min / max / sum across the whole buffer.
    BufferStats getAverageMinMaxSum() {
        std::lock_guard<std::mutex> guard(lock_);
        return getAverageMinMaxSumNoLock();
    }

    // Sum/min/max over the most recent numberOfItems entries.
    BufferStats sumMinMaxLast(int numberOfItems) {
        std::lock_guard<std::mutex> guard(lock_);
        int index = position_ - numberOfItems;
        if (index < 0) {
            index += size_;
        }
        double mn = std::numeric_limits<double>::max();
        double mx = 0.0;
        double sum = 0.0;
        int remaining = numberOfItems;
        while (remaining > 0) {
            double x = data_[index];
            sum += x;
            if (x > mx) mx = x;
            if (x < mn) mn = x;
            index += 1;
            if (index == size_) index = 0;
            remaining -= 1;
        }
        return BufferStats{0.0, mn, mx, sum};
    }

    // Average over the most recent numberOfItems entries.
    double averageLast(int numberOfItems) {
        std::lock_guard<std::mutex> guard(lock_);
        int index = position_ - numberOfItems;
        if (index < 0) {
            index += size_;
        }
        int items = numberOfItems;
        double sum = 0.0;
        while (numberOfItems > 0) {
            sum += data_[index];
            index += 1;
            if (index == size_) index = 0;
            numberOfItems -= 1;
        }
        return sum / static_cast<double>(items);
    }

    // The most recently inserted value.
    double getLast() {
        std::lock_guard<std::mutex> guard(lock_);
        int index = position_ - 1;
        if (index < 0) {
            index += size_;
        }
        return data_[index];
    }

    // Copy of the raw backing data plus size and current position. Used by the
    // gust calculation which walks the buffer directly.
    std::vector<double> getRawData(int &outSize, int &outPosition) {
        std::lock_guard<std::mutex> guard(lock_);
        outSize = size_;
        outPosition = position_;
        return data_;
    }

    int getSize() const { return size_; }

    // Reset the buffer to a new size, discarding all existing data.
    // Thread-safe: takes the internal lock. Used to dynamically grow the
    // direction buffer when the user raises the smoothing window via telnet.
    void resize(int newSize) {
        std::lock_guard<std::mutex> guard(lock_);
        size_     = newSize;
        position_ = 0;
        first_    = true;
        data_.assign(newSize, 0.0);
    }

private:
    void addItemNoLock(double val) {
        data_[position_] = val;
        position_ += 1;
        if (position_ == size_) {
            position_ = 0;
        }
        if (first_) {
            for (int i = 0; i < size_; i++) {
                data_[i] = val;
            }
            first_ = false;
        }
    }

    BufferStats getAverageMinMaxSumNoLock() {
        double mn = std::numeric_limits<double>::max();
        double mx = 0.0;
        double sum = 0.0;
        for (double x : data_) {
            if (x > mx) mx = x;
            if (x < mn) mn = x;
            sum += x;
        }
        return BufferStats{sum / static_cast<double>(size_), mn, mx, sum};
    }

    int position_;
    int size_;
    bool first_;
    std::vector<double> data_;
    std::mutex lock_;
};
