#include "sdl_display.hpp"

#include <cctype>
#include <cstdio>

namespace famidec {

namespace {

// Minimal 5x7 bitmap font (rows top-down, 5 LSBs used, bit4 = left column).
const uint8_t* glyph5x7(char c) {
    static const uint8_t kDigits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    };
    static const uint8_t kColon[7] = {0x00,0x04,0x00,0x00,0x04,0x00,0x00};
    static const uint8_t kDot[7]   = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
    static const uint8_t kDash[7]  = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t kSpace[7] = {0,0,0,0,0,0,0};
    static const uint8_t kLetters[26][7] = {
        /*A*/{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
        /*B*/{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        /*C*/{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
        /*D*/{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
        /*E*/{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
        /*F*/{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        /*G*/{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
        /*H*/{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        /*I*/{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
        /*J*/{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
        /*K*/{0x11,0x12,0x14,0x18,0x14,0x12,0x11},
        /*L*/{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        /*M*/{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
        /*N*/{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
        /*O*/{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        /*P*/{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        /*Q*/{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
        /*R*/{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        /*S*/{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
        /*T*/{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        /*U*/{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
        /*V*/{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        /*W*/{0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
        /*X*/{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        /*Y*/{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
        /*Z*/{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    };
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
    switch (c) {
        case ':': return kColon;
        case '.': return kDot;
        case '-': return kDash;
        default: return kSpace;
    }
}

constexpr int kFontScale = 2;
constexpr int kCharW = 6 * kFontScale;  // 5 px glyph + 1 px spacing

void draw_text(SDL_Renderer* ren, int x, int y, const std::string& text,
               uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (size_t ci = 0; ci < text.size(); ++ci) {
        const uint8_t* gl = glyph5x7(text[ci]);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gl[row] & (0x10 >> col)) {
                    SDL_Rect px{x + static_cast<int>(ci) * kCharW +
                                    col * kFontScale,
                                y + row * kFontScale, kFontScale, kFontScale};
                    SDL_RenderFillRect(ren, &px);
                }
    }
}

}  // namespace

bool SdlDisplay::init(const std::string& title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    win_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, Frame::kWidth,
                            Frame::kHeight,
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win_) return false;
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED |
                                            SDL_RENDERER_PRESENTVSYNC);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, 0);
    if (!ren_) return false;
    SDL_RenderSetLogicalSize(ren_, Frame::kWidth, Frame::kHeight);
    tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888,
                             SDL_TEXTUREACCESS_STREAMING, Frame::kWidth,
                             Frame::kHeight);
    return tex_ != nullptr;
}

SdlDisplay::~SdlDisplay() {
    if (tex_) SDL_DestroyTexture(tex_);
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    SDL_Quit();
}

KeyAction SdlDisplay::poll() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return KeyAction::Quit;
        if (ev.type == SDL_KEYDOWN) {
            bool shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
            switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:
                    return KeyAction::Quit;
                case SDLK_l:
                    return shift ? KeyAction::GainLnaDown : KeyAction::GainLnaUp;
                case SDLK_g:
                    return shift ? KeyAction::GainVgaDown : KeyAction::GainVgaUp;
                case SDLK_c:
                    return KeyAction::ToggleColor;
                case SDLK_s:
                    return KeyAction::Screenshot;
                default:
                    break;
            }
        }
    }
    return KeyAction::None;
}

void SdlDisplay::render(const Frame* frame, const OsdStats& stats) {
    if (frame) {
        SDL_UpdateTexture(tex_, nullptr, frame->rgba.data(),
                          Frame::kWidth * 4);
        last_frame_ = *frame;
        have_frame_ = true;
    }
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
    // Minimal OSD: lock indicator (green/red square) and ring-fill bar.
    SDL_Rect lock{8, 8, 12, 12};
    if (stats.line_locked)
        SDL_SetRenderDrawColor(ren_, 0, 220, 0, 255);
    else
        SDL_SetRenderDrawColor(ren_, 220, 0, 0, 255);
    SDL_RenderFillRect(ren_, &lock);
    SDL_Rect burst{24, 8, 12, 12};
    if (stats.burst_amp >= 3.0f)
        SDL_SetRenderDrawColor(ren_, 0, 220, 0, 255);
    else
        SDL_SetRenderDrawColor(ren_, 180, 180, 0, 255);
    SDL_RenderFillRect(ren_, &burst);
    SDL_Rect fill_bg{8, 26, 100, 6};
    SDL_SetRenderDrawColor(ren_, 60, 60, 60, 255);
    SDL_RenderFillRect(ren_, &fill_bg);
    SDL_Rect fill{8, 26, static_cast<int>(stats.ring_fill * 100.0f), 6};
    SDL_SetRenderDrawColor(ren_, 0, 160, 220, 255);
    SDL_RenderFillRect(ren_, &fill);
    // Top-right status text (yellow).
    char line[80];
    if (stats.channel > 0)
        std::snprintf(line, sizeof(line),
                      "V-SYNC:%s H-SYNC:%s VHF:%.2fMHz %dCH",
                      stats.vsync_locked ? "OK" : "--",
                      stats.line_locked ? "OK" : "--", stats.freq_mhz,
                      stats.channel);
    else
        std::snprintf(line, sizeof(line), "V-SYNC:%s H-SYNC:%s VHF:%.2fMHz",
                      stats.vsync_locked ? "OK" : "--",
                      stats.line_locked ? "OK" : "--", stats.freq_mhz);
    std::string text(line);
    int tx = Frame::kWidth - static_cast<int>(text.size()) * kCharW - 8;
    draw_text(ren_, tx, 8, text, 255, 220, 0);
    SDL_RenderPresent(ren_);
}

bool SdlDisplay::screenshot(const Frame& frame, const std::string& path) {
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<uint32_t*>(frame.rgba.data()), Frame::kWidth,
        Frame::kHeight, 32, Frame::kWidth * 4, SDL_PIXELFORMAT_ABGR8888);
    if (!surf) return false;
    bool ok = SDL_SaveBMP(surf, path.c_str()) == 0;
    SDL_FreeSurface(surf);
    return ok;
}

}  // namespace famidec
