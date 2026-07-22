#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "dsp/am_detector.hpp"
#include "dsp/fm_audio.hpp"
#include "dsp/dc_blocker.hpp"
#include "dsp/fir.hpp"
#include "dsp/frame.hpp"
#include "dsp/nco.hpp"
#include "dsp/ntsc_decoder.hpp"
#include "source/baseband_file_source.hpp"
#include "source/file_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/sample_source.hpp"
#include "ui/sdl_audio.hpp"
#include "ui/sdl_display.hpp"
#include "util/spectrum.hpp"

using namespace famidec;

namespace {

void usage() {
    std::printf(
        "famidec - Famicom RF (NTSC-J) HackRF One decoder\n\n"
        "usage: famidec [options]\n"
        "  --channel 1|2         Japan VHF channel preset (default 1)\n"
        "  --freq HZ             explicit video carrier frequency\n"
        "  --input hackrf|file|baseband-f32\n"
        "                        input source (default hackrf)\n"
        "  --file PATH           .cs8 IQ or little-endian f32 baseband file\n"
        "  --loop                loop file playback\n"
        "  --rate HZ             sample rate (default 10e6)\n"
        "  --offset HZ           tuning offset above carrier (default 2e6)\n"
        "  --lna N --vga N --amp gain settings\n"
        "  --mode color|gray     decode mode (default color)\n"
        "  --detector envelope|sync  AM detector (default envelope)\n"
        "  --sat F --hue DEG     color trims\n"
        "  --overscan F          horizontal crop per side 0..0.15 (default 0.047)\n"
        "  --fm-freq HZ          FM radio station for F key (default 80.0e6 TOKYO FM)\n"
        "  --no-audio            disable FM audio output\n"
        "  --volume F            audio volume 0..1 (default 0.7)\n"
        "  --record PATH         tee raw IQ to .cs8 while decoding\n"
        "  --dump-composite PATH write post-AGC composite as f32\n"
        "  --baseband-polarity normal|inverted\n"
        "  --baseband-gain F     multiply baseband samples (default 1)\n"
        "  --baseband-offset F   add after polarity/gain (default 0)\n"
        "  --agc auto|fixed      baseband level mapping (default auto)\n"
        "  --sync-level F        fixed-AGC sync tip after transforms\n"
        "  --blank-level F       fixed-AGC blanking after transforms\n"
        "  --dump-frames PREFIX  write decoded frames as PPM (headless)\n"
        "  --frames N            number of frames for --dump-frames (default 30)\n"
        "  --spectrum            print PSD and exit (no video)\n"
        "\nkeys: q/ESC quit, l/L LNA +/-, g/G VGA +/-, c color toggle, s screenshot\n");
}

bool finite_double(double value) {
    uint64_t bits = std::bit_cast<uint64_t>(value);
    return (bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}

bool finite_float(float value) {
    uint32_t bits = std::bit_cast<uint32_t>(value);
    return (bits & 0x7f800000U) != 0x7f800000U;
}

bool parse_double_value(const char* text, const char* name, double* out) {
    errno = 0;
    char* end = nullptr;
    double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE ||
        !finite_double(value)) {
        std::fprintf(stderr, "invalid finite number for %s: %s\n", name, text);
        return false;
    }
    *out = value;
    return true;
}

bool parse_float_value(const char* text, const char* name, float* out) {
    errno = 0;
    char* end = nullptr;
    float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE ||
        !finite_float(value)) {
        std::fprintf(stderr, "invalid finite number for %s: %s\n", name, text);
        return false;
    }
    *out = value;
    return true;
}

bool parse_args(int argc, char** argv, Config* cfg) {
    bool input_explicit = false;
    bool rate_explicit = false;
    bool baseband_option = false;
    bool sync_level_explicit = false;
    bool blank_level_explicit = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--channel") {
            int ch = std::atoi(next("--channel"));
            if (ch == 1) cfg->video_carrier_hz = 91.25e6;
            else if (ch == 2) cfg->video_carrier_hz = 97.25e6;
            else { std::fprintf(stderr, "channel must be 1 or 2\n"); return false; }
        } else if (a == "--freq") {
            if (!parse_double_value(next("--freq"), "--freq",
                                    &cfg->video_carrier_hz))
                return false;
        } else if (a == "--input") {
            std::string v = next("--input");
            if (v == "hackrf") cfg->input = Config::Input::HackRF;
            else if (v == "file") cfg->input = Config::Input::File;
            else if (v == "baseband-f32")
                cfg->input = Config::Input::BasebandF32;
            else {
                std::fprintf(
                    stderr,
                    "--input must be hackrf, file, or baseband-f32\n");
                return false;
            }
            input_explicit = true;
        } else if (a == "--file") {
            cfg->file_path = next("--file");
            if (!input_explicit) cfg->input = Config::Input::File;
        } else if (a == "--loop") cfg->loop = true;
        else if (a == "--rate") {
            if (!parse_double_value(next("--rate"), "--rate", &cfg->sample_rate))
                return false;
            rate_explicit = true;
        } else if (a == "--offset") {
            if (!parse_double_value(next("--offset"), "--offset", &cfg->offset_hz))
                return false;
        } else if (a == "--lna") cfg->lna_gain = std::atoi(next("--lna"));
        else if (a == "--vga") cfg->vga_gain = std::atoi(next("--vga"));
        else if (a == "--amp") cfg->amp = true;
        else if (a == "--mode") {
            std::string v = next("--mode");
            if (v == "gray") cfg->mode = Config::Mode::Gray;
            else if (v == "color") cfg->mode = Config::Mode::Color;
            else {
                std::fprintf(stderr, "--mode must be color or gray\n");
                return false;
            }
        } else if (a == "--detector") {
            std::string v = next("--detector");
            if (v == "sync") cfg->detector = Config::Detector::SyncPLL;
            else if (v == "envelope") cfg->detector = Config::Detector::Envelope;
            else {
                std::fprintf(stderr, "--detector must be envelope or sync\n");
                return false;
            }
        } else if (a == "--no-audio") cfg->audio = false;
        else if (a == "--volume") {
            if (!parse_float_value(next("--volume"), "--volume", &cfg->volume))
                return false;
        } else if (a == "--fm-freq") {
            if (!parse_double_value(next("--fm-freq"), "--fm-freq",
                                    &cfg->fm_freq_hz))
                return false;
        } else if (a == "--overscan") {
            if (!parse_float_value(next("--overscan"), "--overscan", &cfg->overscan))
                return false;
        } else if (a == "--sat") {
            if (!parse_float_value(next("--sat"), "--sat", &cfg->saturation))
                return false;
        } else if (a == "--hue") {
            if (!parse_float_value(next("--hue"), "--hue", &cfg->hue_deg))
                return false;
        } else if (a == "--baseband-polarity") {
            std::string v = next("--baseband-polarity");
            if (v == "normal") cfg->baseband_invert = false;
            else if (v == "inverted") cfg->baseband_invert = true;
            else {
                std::fprintf(stderr,
                             "--baseband-polarity must be normal or inverted\n");
                return false;
            }
            baseband_option = true;
        } else if (a == "--baseband-gain") {
            if (!parse_float_value(next("--baseband-gain"), "--baseband-gain",
                                   &cfg->baseband_gain))
                return false;
            baseband_option = true;
        } else if (a == "--baseband-offset") {
            if (!parse_float_value(next("--baseband-offset"), "--baseband-offset",
                                   &cfg->baseband_offset))
                return false;
            baseband_option = true;
        } else if (a == "--agc") {
            std::string v = next("--agc");
            if (v == "auto") cfg->baseband_agc = Config::AgcMode::Auto;
            else if (v == "fixed") cfg->baseband_agc = Config::AgcMode::Fixed;
            else {
                std::fprintf(stderr, "--agc must be auto or fixed\n");
                return false;
            }
            baseband_option = true;
        } else if (a == "--sync-level") {
            if (!parse_float_value(next("--sync-level"), "--sync-level",
                                   &cfg->baseband_sync_level))
                return false;
            sync_level_explicit = true;
            baseband_option = true;
        } else if (a == "--blank-level") {
            if (!parse_float_value(next("--blank-level"), "--blank-level",
                                   &cfg->baseband_blank_level))
                return false;
            blank_level_explicit = true;
            baseband_option = true;
        } else if (a == "--record") cfg->record_path = next("--record");
        else if (a == "--dump-composite") cfg->dump_composite_path = next("--dump-composite");
        else if (a == "--dump-frames") { cfg->dump_frames_prefix = next("--dump-frames"); cfg->headless = true; }
        else if (a == "--frames") cfg->dump_frame_count = std::atoi(next("--frames"));
        else if (a == "--spectrum") cfg->spectrum = true;
        else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    if (cfg->input == Config::Input::File && cfg->file_path.empty()) {
        std::fprintf(stderr, "--input file requires --file PATH\n");
        return false;
    }
    if (!(cfg->sample_rate > 0.0)) {
        std::fprintf(stderr, "--rate must be greater than zero\n");
        return false;
    }
    if (cfg->input == Config::Input::BasebandF32) {
        if (cfg->file_path.empty()) {
            std::fprintf(stderr, "--input baseband-f32 requires --file PATH\n");
            return false;
        }
        if (!rate_explicit) {
            std::fprintf(stderr,
                         "--input baseband-f32 requires an explicit --rate HZ\n");
            return false;
        }
        if (cfg->spectrum) {
            std::fprintf(stderr, "--spectrum is only available for IQ input\n");
            return false;
        }
        if (!cfg->record_path.empty()) {
            std::fprintf(
                stderr,
                "--record records IQ and is unavailable for baseband input\n");
            return false;
        }
        if (cfg->baseband_agc == Config::AgcMode::Fixed) {
            if (!sync_level_explicit || !blank_level_explicit) {
                std::fprintf(stderr,
                             "--agc fixed requires --sync-level and --blank-level\n");
                return false;
            }
            if (!(cfg->baseband_sync_level > cfg->baseband_blank_level)) {
                std::fprintf(stderr,
                             "fixed AGC requires sync level greater than blank level\n");
                return false;
            }
        } else if (sync_level_explicit || blank_level_explicit) {
            std::fprintf(
                stderr,
                "--sync-level/--blank-level require --agc fixed\n");
            return false;
        }
    } else if (baseband_option) {
        std::fprintf(stderr, "baseband options require --input baseband-f32\n");
        return false;
    }
    if (cfg->headless && cfg->dump_frame_count <= 0) cfg->dump_frame_count = 30;
    return true;
}

bool write_ppm(const Frame& f, const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    bool ok = std::fprintf(fp, "P6\n%d %d\n255\n", Frame::kWidth,
                           Frame::kHeight) > 0;
    for (uint32_t px : f.rgba) {
        uint8_t rgb[3] = {static_cast<uint8_t>(px & 0xff),
                          static_cast<uint8_t>((px >> 8) & 0xff),
                          static_cast<uint8_t>((px >> 16) & 0xff)};
        if (std::fwrite(rgb, 1, 3, fp) != 3) {
            ok = false;
            break;
        }
    }
    if (std::fclose(fp) != 0) ok = false;
    return ok;
}

std::atomic<bool> g_running{true};
std::atomic<bool> g_radio{false};  // broadcast FM radio mode (F key)
std::atomic<bool> g_input_error{false};

// IQ recorder shared between the DSP thread (writer) and the main loop
// (start/stop via the V key or --record). Writes are 64 KiB blocks, so a
// plain mutex is cheap enough.
class Recorder {
public:
    bool start(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fp_) return false;
        fp_ = std::fopen(path.c_str(), "wb");
        if (!fp_) return false;
        path_ = path;
        bytes_ = 0;
        started_ = std::chrono::steady_clock::now();
        return true;
    }

    void write(const uint8_t* data, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_) return;
        std::fwrite(data, 1, n, fp_);
        bytes_ += n;
    }

    // Returns false if not recording.
    bool stop(std::string* path, uint64_t* bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_) return false;
        std::fclose(fp_);
        fp_ = nullptr;
        if (path) *path = path_;
        if (bytes) *bytes = bytes_;
        return true;
    }

    bool active() {
        std::lock_guard<std::mutex> lk(mu_);
        return fp_ != nullptr;
    }

    float seconds() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_) return 0.0f;
        return std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                            started_)
            .count();
    }

private:
    std::mutex mu_;
    std::FILE* fp_ = nullptr;
    std::string path_;
    uint64_t bytes_ = 0;
    std::chrono::steady_clock::time_point started_;
};

std::string next_recording_path() {
    for (int i = 1; i < 1000; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "famidec_rec_%03d.cs8", i);
        std::FILE* f = std::fopen(name, "rb");
        if (f) {
            std::fclose(f);
            continue;
        }
        return name;
    }
    return "famidec_rec_overflow.cs8";
}

// cs8 IQ bytes -> complex -> DC block -> mix carrier to 0 Hz -> channel LPF
// -> AM detect -> NtscDecoder.
void iq_dsp_thread(const Config& cfg, ISampleSource* src, NtscDecoder* dec,
                   Recorder* rec, FmAudioDemod* fm_tv,
                   FmAudioDemod* fm_radio, SdlAudioOut* aout) {
    constexpr size_t kBlockBytes = 1 << 16;  // 32768 complex samples
    std::vector<uint8_t> raw(kBlockBytes);
    std::vector<std::complex<float>> iq(kBlockBytes / 2);
    std::vector<float> comp(kBlockBytes / 2);

    DcBlocker dcb;
    Nco mixer(cfg.offset_hz, cfg.sample_rate);  // shift -offset..: see below
    FirFilterC chan_lpf(
        [&] {
            auto t = design_lowpass(4.3e6, cfg.sample_rate, 63);
            return t;
        }());
    EnvelopeDetector env;
    SyncDetector sync_det(cfg.sample_rate);

    // Carrier sits at -offset in the IQ band; multiply by e^{+j*2pi*offset*t}
    // to bring it to 0 Hz.
    while (g_running.load(std::memory_order_relaxed)) {
        size_t n = src->read(raw.data(), raw.size());
        if (n == 0) break;
        rec->write(raw.data(), n);
        size_t ns = n / 2;
        for (size_t i = 0; i < ns; ++i) {
            std::complex<float> c(
                static_cast<int8_t>(raw[2 * i]) / 128.0f,
                static_cast<int8_t>(raw[2 * i + 1]) / 128.0f);
            iq[i] = dcb.process(c) * mixer.next();
        }
        FmAudioDemod* fm =
            g_radio.load(std::memory_order_relaxed) ? fm_radio : fm_tv;
        if (fm && aout) {
            static thread_local std::vector<float> audio;
            audio.clear();
            fm->process(iq.data(), ns, audio);
            aout->push(audio.data(), audio.size());
        }
        chan_lpf.process(iq.data(), iq.data(), ns);
        if (cfg.detector == Config::Detector::SyncPLL)
            sync_det.process(iq.data(), comp.data(), ns);
        else
            env.process(iq.data(), comp.data(), ns);
        dec->process(comp.data(), ns);
    }
    g_running.store(false, std::memory_order_relaxed);
}

// Detected-composite input goes straight to the video decoder. In particular,
// it must not pass through IQ DC blocking, tuning, filtering, AM detection,
// audio demodulation, or spectrum analysis.
void baseband_dsp_thread(BasebandFileSource* src, NtscDecoder* dec) {
    constexpr size_t kBlockSamples = 1 << 15;
    std::vector<float> samples(kBlockSamples);
    while (g_running.load(std::memory_order_relaxed)) {
        size_t n = src->read(samples.data(), samples.size());
        if (n == 0) {
            if (!src->error().empty()) {
                std::fprintf(stderr, "baseband input error: %s\n",
                             src->error().c_str());
                g_input_error.store(true, std::memory_order_relaxed);
            }
            break;
        }
        dec->process(samples.data(), n);
    }
    g_running.store(false, std::memory_order_relaxed);
}

int run_spectrum(const Config& cfg, ISampleSource* src) {
    // Grab ~0.5 s of IQ and print the PSD.
    size_t n_samples = static_cast<size_t>(cfg.sample_rate / 2);
    std::vector<uint8_t> buf(n_samples * 2);
    size_t got = src->read(buf.data(), buf.size());
    if (got < 4096) {
        std::fprintf(stderr, "not enough samples for PSD\n");
        return 1;
    }
    print_psd(reinterpret_cast<const int8_t*>(buf.data()), got / 2,
              cfg.center_hz(), cfg.sample_rate);
    std::printf("\nexpect: video carrier at %.3f MHz (-%.1f MHz from center), "
                "chroma at %.3f MHz, audio at %.3f MHz\n",
                cfg.video_carrier_hz / 1e6, cfg.offset_hz / 1e6,
                (cfg.video_carrier_hz + 3.579545e6) / 1e6,
                (cfg.video_carrier_hz + 4.5e6) / 1e6);
    uint64_t clipped = src->clipped_samples();
    if (clipped) std::printf("WARNING: %llu clipped samples - reduce gain\n",
                             static_cast<unsigned long long>(clipped));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, &cfg)) {
        usage();
        return 2;
    }

    std::unique_ptr<ISampleSource> src;
    std::unique_ptr<BasebandFileSource> baseband;
    HackRfSource* hackrf = nullptr;
    if (cfg.input == Config::Input::HackRF) {
        auto h = std::make_unique<HackRfSource>(cfg);
        hackrf = h.get();
        src = std::move(h);
    } else if (cfg.input == Config::Input::File) {
        // Pace file playback only when showing a live window.
        src = std::make_unique<FileSource>(cfg, !cfg.headless && !cfg.spectrum);
    } else {
        baseband = std::make_unique<BasebandFileSource>(cfg, !cfg.headless);
    }
    bool input_started = src ? src->start() : baseband->start();
    if (!input_started) {
        std::string detail;
        if (hackrf) detail = hackrf->error();
        else if (baseband) detail = baseband->error();
        std::fprintf(stderr, "failed to start input source%s%s\n",
                     detail.empty() ? "" : ": ", detail.c_str());
        return 1;
    }
    if (baseband) {
        std::printf("input: baseband-f32 %s   %.6f MSPS   AGC %s\n",
                    cfg.file_path.c_str(), cfg.sample_rate / 1e6,
                    cfg.baseband_agc == Config::AgcMode::Fixed ? "fixed"
                                                               : "auto");
    } else {
        std::printf(
            "input: %s   video carrier %.3f MHz   center %.3f MHz   %.1f MSPS\n",
            cfg.input == Config::Input::HackRF ? "HackRF" : cfg.file_path.c_str(),
            cfg.video_carrier_hz / 1e6, cfg.center_hz() / 1e6,
            cfg.sample_rate / 1e6);
    }

    if (cfg.spectrum) {
        int rc = run_spectrum(cfg, src.get());
        src->stop();
        return rc;
    }

    Recorder rec;
    if (!cfg.record_path.empty() && !rec.start(cfg.record_path)) {
        std::fprintf(stderr, "cannot open %s\n", cfg.record_path.c_str());
        return 1;
    }

    TripleBuffer tb;
    NtscDecoder dec(cfg, tb);

    // FM audio (window mode only): TV intercarrier + broadcast radio demods.
    std::unique_ptr<FmAudioDemod> fm, fm_radio;
    SdlAudioOut aout;
    if (cfg.audio && !cfg.headless && !baseband) {
        auto tv = std::make_unique<FmAudioDemod>(
            FmAudioDemod::tv(cfg.sample_rate, cfg.volume));
        auto radio = std::make_unique<FmAudioDemod>(
            FmAudioDemod::wfm(cfg.sample_rate, cfg.volume));
        if (aout.init(static_cast<int>(tv->out_rate()))) {
            fm = std::move(tv);
            fm_radio = std::move(radio);
        } else {
            std::fprintf(stderr, "audio device unavailable, continuing muted\n");
        }
    }

    std::thread dsp;
    if (baseband) {
        dsp = std::thread(baseband_dsp_thread, baseband.get(), &dec);
    } else {
        dsp = std::thread(iq_dsp_thread, std::cref(cfg), src.get(), &dec, &rec,
                          fm.get(), fm_radio.get(),
                          aout.ok() ? &aout : nullptr);
    }

    int rc = 0;
    if (cfg.headless) {
        // Dump N frames as PPM, then exit.
        uint64_t last_seq = 0;
        int written = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (written < cfg.dump_frame_count &&
               std::chrono::steady_clock::now() < deadline) {
            const Frame* f = tb.acquire();
            if (f && f->seq != last_seq) {
                last_seq = f->seq;
                char path[512];
                std::snprintf(path, sizeof(path), "%s%04d.ppm",
                              cfg.dump_frames_prefix.c_str(), written);
                if (!write_ppm(*f, path)) {
                    std::fprintf(stderr, "cannot write frame %s\n", path);
                    rc = 1;
                    break;
                }
                ++written;
            } else if (!g_running.load(std::memory_order_relaxed)) {
                // The producer has reached EOF. TripleBuffer retains its
                // freshest publication, so check it once before stopping.
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        std::printf("wrote %d frames; decoded lines=%llu coasted=%llu frames=%llu\n",
                    written,
                    static_cast<unsigned long long>(dec.stats().lines.load()),
                    static_cast<unsigned long long>(dec.stats().lines_coasted.load()),
                    static_cast<unsigned long long>(dec.stats().frames.load()));
        if (written == 0) rc = 1;
    } else {
        SdlDisplay disp;
        if (!disp.init("famidec - Famicom RF decoder")) {
            std::fprintf(stderr, "SDL init failed\n");
            g_running.store(false);
            dsp.join();
            return 1;
        }
        Config& mcfg = cfg;
        int shot = 0;
        Frame last_shown;
        bool have_shown = false;
        uint64_t prev_frames = 0;
        auto last_frame_inc = std::chrono::steady_clock::now();
        bool show_help = false;
        bool show_hud = true;
        bool crt_mode = false;
        float fps = 0.0f;
        uint64_t fps_base_frames = 0;
        auto fps_base_time = std::chrono::steady_clock::now();
        // Nearest VHF channel (real modulators drift a few hundred kHz).
        int channel = 0;
        if (std::abs(cfg.video_carrier_hz - 91.25e6) < 3e6) channel = 1;
        else if (std::abs(cfg.video_carrier_hz - 97.25e6) < 3e6) channel = 2;
        while (g_running.load(std::memory_order_relaxed)) {
            KeyAction act = disp.poll();
            if (act == KeyAction::Quit) break;
            if (hackrf) {
                if (act == KeyAction::GainLnaUp) hackrf->set_gains(hackrf->lna() + 8, hackrf->vga());
                if (act == KeyAction::GainLnaDown) hackrf->set_gains(hackrf->lna() - 8, hackrf->vga());
                if (act == KeyAction::GainVgaUp) hackrf->set_gains(hackrf->lna(), hackrf->vga() + 2);
                if (act == KeyAction::GainVgaDown) hackrf->set_gains(hackrf->lna(), hackrf->vga() - 2);
            }
            if (act == KeyAction::ToggleHelp) show_help = !show_help;
            if (act == KeyAction::ToggleRecord && src) {
                std::string p;
                uint64_t b;
                if (rec.stop(&p, &b))
                    std::printf(
                        "saved %s (%.1f MB) - replay: famidec --input file "
                        "--file %s\n",
                        p.c_str(), b / 1e6, p.c_str());
                else if (rec.start(next_recording_path()))
                    std::printf("recording IQ...\n");
                std::fflush(stdout);
            }
            if (act == KeyAction::ToggleCrt) crt_mode = !crt_mode;
            if (act == KeyAction::ToggleHud) show_hud = !show_hud;
            if (act == KeyAction::ToggleRadio && hackrf) {
                bool r = !g_radio.load(std::memory_order_relaxed);
                g_radio.store(r, std::memory_order_relaxed);
                hackrf->set_center_freq(
                    (r ? mcfg.fm_freq_hz : mcfg.video_carrier_hz) +
                    mcfg.offset_hz);
            }
            // Arrow-key tuning: left/right 50 kHz, up/down 1 MHz.
            double tune = 0.0;
            if (act == KeyAction::FreqUp) tune = 50e3;
            if (act == KeyAction::FreqDown) tune = -50e3;
            if (act == KeyAction::FreqUpBig) tune = 1e6;
            if (act == KeyAction::FreqDownBig) tune = -1e6;
            if (tune != 0.0 && hackrf) {
                if (g_radio.load(std::memory_order_relaxed)) {
                    // FM band: 100 kHz fine steps, 1 MHz coarse.
                    mcfg.fm_freq_hz +=
                        (std::abs(tune) == 50e3) ? (tune > 0 ? 1e5 : -1e5)
                                                 : tune;
                    hackrf->set_center_freq(mcfg.fm_freq_hz + mcfg.offset_hz);
                } else {
                    mcfg.video_carrier_hz += tune;
                    hackrf->set_center_freq(mcfg.center_hz());
                    if (std::abs(mcfg.video_carrier_hz - 91.25e6) < 3e6) channel = 1;
                    else if (std::abs(mcfg.video_carrier_hz - 97.25e6) < 3e6) channel = 2;
                    else channel = 0;
                }
            }
            if (act == KeyAction::ToggleColor)
                mcfg.mode = (mcfg.mode == Config::Mode::Color)
                                ? Config::Mode::Gray
                                : Config::Mode::Color;
            const Frame* f = tb.acquire();
            if (f) {
                last_shown = *f;
                have_shown = true;
            }
            if (act == KeyAction::Screenshot && have_shown) {
                char path[64];
                std::snprintf(path, sizeof(path), "famidec_%03d.bmp", shot++);
                disp.screenshot(last_shown, path);
                std::printf("saved %s\n", path);
            }
            OsdStats st;
            st.line_locked = dec.stats().line_locked.load();
            st.burst_amp = dec.stats().burst_amp.load();
            st.ring_fill = src ? src->ring_fill() : 0.0f;
            st.dropped = src ? src->dropped_bytes() : 0;
            st.clipped = src ? src->clipped_samples() : 0;
            st.frames = dec.stats().frames.load();
            if (hackrf) { st.lna = hackrf->lna(); st.vga = hackrf->vga(); }
            // V-SYNC considered locked while real frames keep arriving.
            auto now = std::chrono::steady_clock::now();
            if (st.frames > prev_frames) {
                prev_frames = st.frames;
                last_frame_inc = now;
            }
            st.vsync_locked = (now - last_frame_inc) < std::chrono::milliseconds(500);
            // Decoded-frame rate over a rolling ~1 s window.
            double win = std::chrono::duration<double>(now - fps_base_time).count();
            if (win >= 1.0) {
                fps = static_cast<float>(
                    static_cast<double>(st.frames - fps_base_frames) / win);
                fps_base_frames = st.frames;
                fps_base_time = now;
            }
            st.fps = fps;
            st.show_help = show_help;
            st.crt = crt_mode;
            st.recording = rec.active();
            st.rec_seconds = rec.seconds();
            st.show_hud = show_hud;
            // Video latency: decoder samples since the displayed frame's
            // vsync (same coordinate system, so input drops don't skew it),
            // plus the source's unread backlog and ~one USB transfer (13 ms).
            if (st.vsync_locked) {
                uint64_t dsp_samples = dec.stats().samples_in.load();
                uint64_t vsync_pos = dec.stats().frame_sample_pos.load();
                if (dsp_samples > vsync_pos)
                    st.video_latency_ms = static_cast<float>(
                        (static_cast<double>(dsp_samples - vsync_pos) +
                         static_cast<double>(src ? src->buffered_bytes() / 2
                                                 : baseband->buffered_samples())) /
                            cfg.sample_rate * 1000.0 +
                        (hackrf ? 13.0 : 0.0));
            }
            st.audio_latency_ms = aout.ok() ? aout.queued_ms() : 0.0f;
            st.radio = g_radio.load(std::memory_order_relaxed);
            st.freq_mhz = (st.radio ? cfg.fm_freq_hz : cfg.video_carrier_hz) / 1e6;
            st.audio_mhz = (cfg.video_carrier_hz + 4.5e6) / 1e6;
            st.channel = channel;
            disp.render(f, st);
        }
    }

    g_running.store(false, std::memory_order_relaxed);
    if (baseband) {
        // Baseband reads are ordinary FILE operations and return promptly;
        // never close the stream concurrently with its reader thread.
        dsp.join();
        baseband->stop();
    } else {
        // Live IQ reads may need source shutdown to unblock them.
        src->stop();
        dsp.join();
    }
    std::string rec_path;
    uint64_t rec_bytes = 0;
    if (rec.stop(&rec_path, &rec_bytes))
        std::printf("saved %s (%.1f MB) - replay: famidec --input file --file %s\n",
                    rec_path.c_str(), rec_bytes / 1e6, rec_path.c_str());
    if (g_input_error.load(std::memory_order_relaxed)) rc = 1;
    return rc;
}
