// Independent grayscale composite fixture generator and PPM validator for the
// famidec baseband CLI test. This intentionally does not link to fam_dsp.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::array<float, 5> kBars = {0.0f, 25.0f, 50.0f, 75.0f,
                                        100.0f};

struct FixtureTiming {
    double sample_rate;
    double line_us;
    int lines_per_field;
    int vsync_lines;
    int active_start_line;
    int active_lines;
    double hsync_us;
    double active_start_us;
    double active_width_us;
    int fields;
};

constexpr FixtureTiming kNtscTiming = {
    10e6, 1e6 / 15734.264, 262, 3, 13, 240, 4.7, 9.4, 52.6, 6,
};
constexpr FixtureTiming kCustomTiming = {
    8e6, 80.0, 200, 4, 10, 180, 6.0, 12.0, 60.0, 6,
};

enum class Impairment {
    None,
    ReversedPolarity,
    MissingHsync,
    MalformedVsync,
    ClippedSync,
};

float composite_ire(const FixtureTiming& timing, Impairment impairment,
                    int line, double us) {
    if (line < timing.vsync_lines &&
        impairment != Impairment::MalformedVsync)
        return us > timing.line_us - timing.hsync_us ? 0.0f : -40.0f;
    if (us < timing.hsync_us && impairment != Impairment::MissingHsync)
        return -40.0f;
    if (us < timing.active_start_us ||
        us >= timing.active_start_us + timing.active_width_us)
        return 0.0f;
    int active_line =
        line - timing.vsync_lines - timing.active_start_line;
    if (active_line < 0 || active_line >= timing.active_lines) return 0.0f;
    double frac =
        (us - timing.active_start_us) / timing.active_width_us;
    int bar = std::clamp(static_cast<int>(frac * kBars.size()), 0,
                         static_cast<int>(kBars.size()) - 1);
    return kBars[static_cast<size_t>(bar)];
}

bool generate(const std::string& path, const FixtureTiming& timing,
              Impairment impairment = Impairment::None) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    constexpr size_t kBufferSamples = 8192;
    std::array<char, kBufferSamples * sizeof(float)> buffer{};
    size_t buffered = 0;
    auto flush = [&] {
        out.write(buffer.data(), static_cast<std::streamsize>(buffered));
        buffered = 0;
    };
    auto append_f32le = [&](float value) {
        uint32_t bits = std::bit_cast<uint32_t>(value);
        buffer[buffered++] = static_cast<char>(bits & 0xff);
        buffer[buffered++] = static_cast<char>((bits >> 8) & 0xff);
        buffer[buffered++] = static_cast<char>((bits >> 16) & 0xff);
        buffer[buffered++] = static_cast<char>((bits >> 24) & 0xff);
        if (buffered == buffer.size()) flush();
    };

    const double samples_per_line =
        timing.sample_rate * timing.line_us / 1e6;
    const double samples_per_field =
        samples_per_line * timing.lines_per_field;
    const uint64_t total =
        static_cast<uint64_t>(timing.fields * samples_per_field);
    for (uint64_t n = 0; n < total; ++n) {
        double in_field = std::fmod(static_cast<double>(n), samples_per_field);
        int line = static_cast<int>(in_field / samples_per_line);
        double us = std::fmod(in_field, samples_per_line) /
                    timing.sample_rate * 1e6;
        float ire = composite_ire(timing, impairment, line, us);
        // Negative modulation convention used by NtscDecoder's raw input:
        // sync tip=1.0, blank=0.75, white=0.125.
        float raw = 0.75f - ire * 0.00625f;
        if (impairment == Impairment::ReversedPolarity) raw = -raw;
        if (impairment == Impairment::ClippedSync) raw = std::min(raw, 0.8f);
        append_f32le(raw);
    }
    if (buffered != 0) flush();
    if (!out) return false;
    std::printf("generated %llu baseband samples\n",
                static_cast<unsigned long long>(total));
    return true;
}

bool validate(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int width = 0, height = 0, max_value = 0;
    in >> magic >> width >> height >> max_value;
    if (!in || magic != "P6" || width != 640 || height != 480 ||
        max_value != 255) {
        std::fprintf(stderr, "invalid PPM header\n");
        return false;
    }
    in.get();  // consume the single whitespace byte after max_value
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    in.read(reinterpret_cast<char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
    if (in.gcount() != static_cast<std::streamsize>(pixels.size())) {
        std::fprintf(stderr, "truncated PPM pixels\n");
        return false;
    }

    int failures = 0;
    constexpr int kTolerance = 8;
    for (size_t bar = 0; bar < kBars.size(); ++bar) {
        int x = static_cast<int>((static_cast<double>(bar) + 0.5) /
                                 kBars.size() * width);
        int y = height / 2;
        size_t pos = (static_cast<size_t>(y) * width + x) * 3;
        int expected = static_cast<int>(kBars[bar] / 100.0f * 255.0f + 0.5f);
        int r = pixels[pos], g = pixels[pos + 1], b = pixels[pos + 2];
        bool ok = std::abs(r - expected) <= kTolerance &&
                  std::abs(g - expected) <= kTolerance &&
                  std::abs(b - expected) <= kTolerance;
        std::printf("gray bar %zu: got (%d,%d,%d), expected %d %s\n", bar, r,
                    g, b, expected, ok ? "OK" : "FAIL");
        if (!ok) ++failures;
    }
    return failures == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(
            stderr,
            "usage: baseband_fixture generate|generate-custom|generate-*|validate PATH\n");
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "generate") return generate(argv[2], kNtscTiming) ? 0 : 1;
    if (mode == "generate-custom")
        return generate(argv[2], kCustomTiming) ? 0 : 1;
    if (mode == "generate-reversed")
        return generate(argv[2], kCustomTiming,
                        Impairment::ReversedPolarity) ? 0 : 1;
    if (mode == "generate-missing-hsync")
        return generate(argv[2], kCustomTiming,
                        Impairment::MissingHsync) ? 0 : 1;
    if (mode == "generate-malformed-vsync")
        return generate(argv[2], kCustomTiming,
                        Impairment::MalformedVsync) ? 0 : 1;
    if (mode == "generate-clipped")
        return generate(argv[2], kCustomTiming,
                        Impairment::ClippedSync) ? 0 : 1;
    if (mode == "generate-period-error") {
        FixtureTiming wrong_period = kCustomTiming;
        wrong_period.line_us = 90.0;
        return generate(argv[2], wrong_period) ? 0 : 1;
    }
    if (mode == "validate") return validate(argv[2]) ? 0 : 1;
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
