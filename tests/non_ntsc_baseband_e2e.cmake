cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FAMIDEC OR NOT DEFINED FIXTURE_TOOL OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR
        "non_ntsc_baseband_e2e requires FAMIDEC, FIXTURE_TOOL, and WORK_DIR")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(FIXTURE_PATH "${WORK_DIR}/custom-gray-bars.f32")
set(FRAME_PREFIX "${WORK_DIR}/frame-")
set(FRAME_PATH "${FRAME_PREFIX}0000.ppm")
set(STATS_PATH "${WORK_DIR}/stats.json")
file(REMOVE "${FIXTURE_PATH}" "${FRAME_PATH}" "${STATS_PATH}")

execute_process(
    COMMAND "${FIXTURE_TOOL}" generate-custom "${FIXTURE_PATH}"
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
        --line-rate 12500
        --hsync-min-us 5
        --hsync-max-us 7
        --hsync-scan-us 8
        --acquisition-skip-us 40
        --tracking-window-us 3
        --vsync-fraction 0.6
        --vsync-min-lines 3
        --lines-per-frame 200
        --active-start-line 10
        --active-lines 180
        --active-start-us 12
        --active-width-us 60
        --agc-porch-start-us 8
        --agc-porch-end-us 10
        --dump-frames "${FRAME_PREFIX}"
        --frames 1
        --stats-json "${STATS_PATH}"
    RESULT_VARIABLE decode_result
    OUTPUT_VARIABLE decode_output
    ERROR_VARIABLE decode_error
)
if(NOT decode_result EQUAL 0)
    message(FATAL_ERROR
        "custom-profile decode failed:\n${decode_output}${decode_error}")
endif()
if(NOT EXISTS "${FRAME_PATH}")
    message(FATAL_ERROR
        "custom-profile decode produced no PPM:\n${decode_output}${decode_error}")
endif()
if(NOT EXISTS "${STATS_PATH}")
    message(FATAL_ERROR "custom-profile decode produced no statistics JSON")
endif()

execute_process(
    COMMAND "${FIXTURE_TOOL}" validate "${FRAME_PATH}"
    RESULT_VARIABLE validate_result
    OUTPUT_VARIABLE validate_output
    ERROR_VARIABLE validate_error
)
if(NOT validate_result EQUAL 0)
    message(FATAL_ERROR
        "custom-profile PPM validation failed:\n${validate_output}${validate_error}")
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
    message(FATAL_ERROR "custom-profile statistics do not report lock")
endif()
if(line_rate LESS 12499 OR line_rate GREATER 12501)
    message(FATAL_ERROR "measured line rate ${line_rate} is outside tolerance")
endif()
if(frame_rate LESS 62 OR frame_rate GREATER 63)
    message(FATAL_ERROR "measured frame rate ${frame_rate} is outside tolerance")
endif()
if(sync_width LESS 5.8 OR sync_width GREATER 6.2)
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
