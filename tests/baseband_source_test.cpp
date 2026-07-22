#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "config.hpp"
#include "source/baseband_file_source.hpp"

using namespace famidec;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void write_f32le(const std::filesystem::path& path,
                 const std::vector<float>& samples) {
    std::ofstream out(path, std::ios::binary);
    for (float sample : samples) {
        uint32_t bits = std::bit_cast<uint32_t>(sample);
        const char bytes[4] = {
            static_cast<char>(bits & 0xff),
            static_cast<char>((bits >> 8) & 0xff),
            static_cast<char>((bits >> 16) & 0xff),
            static_cast<char>((bits >> 24) & 0xff),
        };
        out.write(bytes, sizeof(bytes));
    }
}

Config config_for(const std::filesystem::path& path) {
    Config cfg;
    cfg.input = Config::Input::BasebandF32;
    cfg.file_path = path.string();
    cfg.sample_rate = 1e6;
    return cfg;
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("famidec-baseband-test-" + std::to_string(nonce));
    std::filesystem::create_directory(dir);

    const auto valid = dir / "valid.f32";
    write_f32le(valid, {1.0f, -2.0f, 0.25f});
    Config transformed = config_for(valid);
    transformed.baseband_invert = true;
    transformed.baseband_gain = 2.0f;
    transformed.baseband_offset = 0.5f;
    BasebandFileSource source(transformed, false);
    check(source.start(), "valid source starts");
    float samples[4] = {};
    check(source.read(samples, 4) == 3, "valid source reaches clean EOF");
    check(std::abs(samples[0] - -1.5f) < 1e-6f &&
              std::abs(samples[1] - 4.5f) < 1e-6f &&
              std::abs(samples[2] - 0.0f) < 1e-6f,
          "polarity, gain, and offset are applied in documented order");
    check(source.total_samples() == 3 && source.file_samples() == 3,
          "source reports sample counts");
    source.stop();

    Config looped = config_for(valid);
    looped.loop = true;
    BasebandFileSource loop_source(looped, false);
    check(loop_source.start(), "looped source starts");
    float repeated[5] = {};
    check(loop_source.read(repeated, 5) == 5, "loop fills requested block");
    check(repeated[0] == 1.0f && repeated[1] == -2.0f &&
              repeated[2] == 0.25f && repeated[3] == 1.0f &&
              repeated[4] == -2.0f,
          "loop restarts at the first sample");

    const auto empty = dir / "empty.f32";
    std::ofstream(empty, std::ios::binary);
    Config empty_cfg = config_for(empty);
    BasebandFileSource empty_source(empty_cfg, false);
    check(!empty_source.start() && empty_source.error().find("empty") != std::string::npos,
          "empty input is rejected explicitly");

    const auto truncated = dir / "truncated.f32";
    {
        std::ofstream out(truncated, std::ios::binary);
        out.write("abc", 3);
    }
    Config truncated_cfg = config_for(truncated);
    BasebandFileSource truncated_source(truncated_cfg, false);
    check(!truncated_source.start() &&
              truncated_source.error().find("multiple of 4") != std::string::npos,
          "truncated input is rejected explicitly");

    const auto nonfinite = dir / "nonfinite.f32";
    write_f32le(nonfinite, {0.0f, std::numeric_limits<float>::quiet_NaN()});
    Config nonfinite_cfg = config_for(nonfinite);
    BasebandFileSource nonfinite_source(nonfinite_cfg, false);
    check(nonfinite_source.start(), "non-finite source passes structural validation");
    float rejected[2] = {};
    check(nonfinite_source.read(rejected, 2) == 0 &&
              nonfinite_source.error().find("index 1") != std::string::npos,
          "non-finite samples fail with their file index");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) return 1;
    std::printf("PASS: baseband source validation and transforms\n");
    return 0;
}
