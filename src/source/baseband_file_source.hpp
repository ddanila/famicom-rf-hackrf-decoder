#pragma once

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "../config.hpp"

namespace famidec {

// Plays raw detected-composite samples stored as little-endian IEEE-754 f32.
// This source deliberately does not implement ISampleSource: baseband samples
// bypass every RF/IQ-only DSP stage.
class BasebandFileSource {
public:
    BasebandFileSource(const Config& cfg, bool pace_realtime)
        : cfg_(cfg), pace_(pace_realtime) {}
    ~BasebandFileSource() { stop(); }

    bool start() {
        stop();
        error_.clear();
        total_samples_ = 0;
        file_sample_index_ = 0;
        fp_ = std::fopen(cfg_.file_path.c_str(), "rb");
        if (!fp_) {
            error_ = "cannot open baseband file '" + cfg_.file_path + "'";
            return false;
        }
        if (std::fseek(fp_, 0, SEEK_END) != 0) {
            error_ = "cannot determine baseband file length";
            stop();
            return false;
        }
        long bytes = std::ftell(fp_);
        if (bytes < 0) {
            error_ = "cannot determine baseband file length";
            stop();
            return false;
        }
        if (bytes == 0) {
            error_ = "baseband file is empty";
            stop();
            return false;
        }
        if (bytes % 4 != 0) {
            error_ = "baseband file length is not a multiple of 4 bytes";
            stop();
            return false;
        }
        file_samples_ = static_cast<uint64_t>(bytes / 4);
        std::rewind(fp_);
        start_time_ = std::chrono::steady_clock::now();
        return true;
    }

    void stop() {
        if (fp_) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    size_t read(float* dst, size_t sample_count) {
        if (!fp_ || !error_.empty()) return 0;
        size_t got = 0;
        while (got < sample_count) {
            uint8_t bytes[4];
            size_t n = std::fread(bytes, 1, sizeof(bytes), fp_);
            if (n == 0 && std::feof(fp_)) {
                if (!cfg_.loop) break;
                std::clearerr(fp_);
                std::rewind(fp_);
                file_sample_index_ = 0;
                continue;
            }
            if (n != sizeof(bytes)) {
                error_ = "failed while reading baseband file";
                return 0;
            }
            uint32_t bits = static_cast<uint32_t>(bytes[0]) |
                            (static_cast<uint32_t>(bytes[1]) << 8) |
                            (static_cast<uint32_t>(bytes[2]) << 16) |
                            (static_cast<uint32_t>(bytes[3]) << 24);
            // Inspect the IEEE-754 exponent directly. The project is built
            // with -ffast-math, under which std::isfinite may be optimized
            // on the assumption that NaN and infinity cannot occur.
            if ((bits & 0x7f800000U) == 0x7f800000U) {
                error_ = "non-finite baseband sample at index " +
                         std::to_string(file_sample_index_);
                return 0;
            }
            float sample = std::bit_cast<float>(bits);
            if (cfg_.baseband_invert) sample = -sample;
            dst[got++] = sample * cfg_.baseband_gain + cfg_.baseband_offset;
            ++file_sample_index_;
        }

        total_samples_ += got;
        if (pace_ && got > 0) {
            double elapsed_target =
                static_cast<double>(total_samples_) / cfg_.sample_rate;
            auto target = start_time_ + std::chrono::duration_cast<
                                            std::chrono::steady_clock::duration>(
                                            std::chrono::duration<double>(
                                                elapsed_target));
            std::this_thread::sleep_until(target);
        }
        return got;
    }

    const std::string& error() const { return error_; }
    uint64_t total_samples() const { return total_samples_; }
    uint64_t file_samples() const { return file_samples_; }
    size_t buffered_samples() const { return 0; }

private:
    const Config& cfg_;
    bool pace_;
    std::FILE* fp_ = nullptr;
    std::string error_;
    uint64_t file_samples_ = 0;
    uint64_t file_sample_index_ = 0;
    uint64_t total_samples_ = 0;
    std::chrono::steady_clock::time_point start_time_;
};

}  // namespace famidec
