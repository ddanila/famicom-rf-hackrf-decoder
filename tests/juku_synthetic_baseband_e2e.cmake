cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FAMIDEC OR NOT DEFINED FIXTURE_TOOL OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR
        "juku_synthetic_baseband_e2e requires FAMIDEC, FIXTURE_TOOL, and WORK_DIR")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(FIXTURE_PATH "${WORK_DIR}/juku-synthetic-gray-bars.f32")
set(FRAME_PREFIX "${WORK_DIR}/frame-")
set(FRAME_PATH "${FRAME_PREFIX}0000.ppm")
set(STATS_PATH "${WORK_DIR}/stats.json")
file(REMOVE "${FIXTURE_PATH}" "${FRAME_PATH}" "${STATS_PATH}")

execute_process(
    COMMAND "${FIXTURE_TOOL}" generate-juku-synthetic "${FIXTURE_PATH}"
    RESULT_VARIABLE fixture_result
    OUTPUT_VARIABLE fixture_output
    ERROR_VARIABLE fixture_error
)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "fixture generation failed:\n${fixture_output}${fixture_error}")
endif()

execute_process(
    COMMAND "${FAMIDEC}"
        --input baseband-f32
        --file "${FIXTURE_PATH}"
        --rate 8000000
        --mode gray
        --overscan 0
        --no-audio
        --agc fixed
        --sync-level 1
        --blank-level 0.75
        --timing custom
        --line-rate 15625
        --hsync-min-us 4.5
        --hsync-max-us 5.6
        --hsync-scan-us 6.5
        --acquisition-skip-us 32
        --tracking-window-us 2
        --vsync-fraction 0.6
        --vsync-min-lines 3
        --lines-per-frame 313
        --active-start-line 44
        --active-lines 241
        --active-start-us 16
        --active-width-us 40
        --agc-porch-start-us 7
        --agc-porch-end-us 9
        --dump-frames "${FRAME_PREFIX}"
        --frames 2
        --stats-json "${STATS_PATH}"
    RESULT_VARIABLE decode_result
    OUTPUT_VARIABLE decode_output
    ERROR_VARIABLE decode_error
)
if(NOT decode_result EQUAL 0)
    message(FATAL_ERROR
        "synthetic Juku-profile decode failed:\n${decode_output}${decode_error}")
endif()
if(NOT EXISTS "${FRAME_PATH}" OR NOT EXISTS "${STATS_PATH}")
    message(FATAL_ERROR
        "synthetic Juku-profile decode omitted frame or statistics output")
endif()

execute_process(
    COMMAND "${FIXTURE_TOOL}" validate "${FRAME_PATH}"
    RESULT_VARIABLE validate_result
    OUTPUT_VARIABLE validate_output
    ERROR_VARIABLE validate_error
)
if(NOT validate_result EQUAL 0)
    message(FATAL_ERROR
        "synthetic Juku-profile PPM validation failed:\n${validate_output}${validate_error}")
endif()

file(READ "${STATS_PATH}" stats_json)
string(JSON timing_policy GET "${stats_json}" timing_policy)
string(JSON line_locked GET "${stats_json}" line_locked)
string(JSON frame_locked GET "${stats_json}" frame_locked)
string(JSON line_rate GET "${stats_json}" measured_line_rate_hz)
string(JSON frame_rate GET "${stats_json}" measured_frame_rate_hz)
string(JSON sync_width GET "${stats_json}" measured_sync_width_us)
string(JSON blank_ire GET "${stats_json}" measured_blank_ire)
string(JSON video_min GET "${stats_json}" measured_video_min_ire)
string(JSON video_max GET "${stats_json}" measured_video_max_ire)
if(NOT timing_policy STREQUAL "custom" OR NOT line_locked OR NOT frame_locked)
    message(FATAL_ERROR "synthetic Juku-profile statistics do not report lock")
endif()
if(line_rate LESS 15624 OR line_rate GREATER 15626)
    message(FATAL_ERROR "measured line rate ${line_rate} is outside tolerance")
endif()
if(frame_rate LESS 49.8 OR frame_rate GREATER 50.1)
    message(FATAL_ERROR "measured frame rate ${frame_rate} is outside tolerance")
endif()
if(sync_width LESS 4.8 OR sync_width GREATER 5.2)
    message(FATAL_ERROR "measured sync width ${sync_width} is outside tolerance")
endif()
if(blank_ire LESS -0.1 OR blank_ire GREATER 0.1)
    message(FATAL_ERROR "measured blank ${blank_ire} IRE is outside tolerance")
endif()
if(video_min GREATER 1 OR video_max LESS 99)
    message(FATAL_ERROR
        "measured video range ${video_min}..${video_max} IRE is incomplete")
endif()

message(STATUS "${decode_output}${validate_output}")
file(REMOVE "${FIXTURE_PATH}" "${FRAME_PATH}" "${STATS_PATH}")
