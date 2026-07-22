cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FAMIDEC OR NOT DEFINED FIXTURE_TOOL OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR
        "non_ntsc_negative requires FAMIDEC, FIXTURE_TOOL, and WORK_DIR")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(CUSTOM_ARGS
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
    --frames 1
)

function(check_negative mode expected_line_lock)
    set(fixture "${WORK_DIR}/${mode}.f32")
    set(frame_prefix "${WORK_DIR}/${mode}-frame-")
    set(frame "${frame_prefix}0000.ppm")
    set(stats "${WORK_DIR}/${mode}-stats.json")
    file(REMOVE "${fixture}" "${frame}" "${stats}")

    execute_process(
        COMMAND "${FIXTURE_TOOL}" "${mode}" "${fixture}"
        RESULT_VARIABLE fixture_result
        OUTPUT_VARIABLE fixture_output
        ERROR_VARIABLE fixture_error
    )
    if(NOT fixture_result EQUAL 0)
        message(FATAL_ERROR
            "${mode} generation failed:\n${fixture_output}${fixture_error}")
    endif()

    execute_process(
        COMMAND "${FAMIDEC}"
            --input baseband-f32 --file "${fixture}"
            ${CUSTOM_ARGS}
            --dump-frames "${frame_prefix}"
            --stats-json "${stats}"
        RESULT_VARIABLE decode_result
        OUTPUT_VARIABLE decode_output
        ERROR_VARIABLE decode_error
    )
    if(decode_result EQUAL 0)
        message(FATAL_ERROR "${mode} incorrectly reported decode success")
    endif()
    if(NOT EXISTS "${stats}")
        message(FATAL_ERROR "${mode} produced no diagnostic statistics")
    endif()
    file(READ "${stats}" stats_json)
    string(JSON line_locked GET "${stats_json}" line_locked)
    string(JSON frame_locked GET "${stats_json}" frame_locked)
    if(frame_locked)
        message(FATAL_ERROR "${mode} incorrectly reported frame lock")
    endif()
    if(expected_line_lock AND NOT line_locked)
        message(FATAL_ERROR "${mode} should retain horizontal lock")
    endif()
    if(NOT expected_line_lock AND line_locked)
        message(FATAL_ERROR "${mode} incorrectly reported horizontal lock")
    endif()
    string(CONCAT combined_output "${decode_output}" "${decode_error}")
    if(NOT combined_output MATCHES "did not achieve frame lock")
        message(FATAL_ERROR "${mode} omitted its frame-lock diagnostic")
    endif()
    file(REMOVE "${fixture}" "${frame}" "${stats}")
endfunction()

check_negative(generate-reversed FALSE)
check_negative(generate-missing-hsync FALSE)
check_negative(generate-malformed-vsync TRUE)
check_negative(generate-clipped FALSE)
check_negative(generate-period-error FALSE)
