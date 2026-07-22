#pragma once

namespace famidec {

// Receiver timing policy, separate from timing measured by the line PLL.
// Defaults preserve the existing Famicom/NTSC-J behavior. Other profiles must
// come from their signal owner; this decoder does not define Juku constants.
struct VideoTimingProfile {
    double nominal_line_rate_hz = 15734.264;

    double hsync_min_us = 3.0;
    double hsync_max_us = 6.0;
    double hsync_scan_limit_us = 7.0;
    double acquisition_skip_us = 30.0;
    double tracking_window_us = 2.0;

    double vsync_asserted_fraction = 0.45;
    int min_vsync_lines = 2;
    int lines_per_frame = 262;

    int active_start_line = 13;
    int active_lines = 240;
    double active_start_us = 9.4;
    double active_width_us = 52.6;

    double agc_back_porch_start_us = 8.2;
    double agc_back_porch_end_us = 9.2;
};

}  // namespace famidec
