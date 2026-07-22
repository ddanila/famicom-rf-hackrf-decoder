cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FAMIDEC OR NOT DEFINED FIXTURE_TOOL OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "baseband_e2e requires FAMIDEC, FIXTURE_TOOL, and WORK_DIR")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(FIXTURE_PATH "${WORK_DIR}/gray-bars.f32")
set(FRAME_PREFIX "${WORK_DIR}/frame-")
set(FRAME_PATH "${FRAME_PREFIX}0000.ppm")
file(REMOVE "${FIXTURE_PATH}" "${FRAME_PATH}")

execute_process(
    COMMAND "${FIXTURE_TOOL}" generate "${FIXTURE_PATH}"
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
        --rate 10000000
        --mode gray
        --overscan 0
        --no-audio
        --agc fixed
        --sync-level 1
        --blank-level 0.75
        --dump-frames "${FRAME_PREFIX}"
        --frames 1
    RESULT_VARIABLE decode_result
    OUTPUT_VARIABLE decode_output
    ERROR_VARIABLE decode_error
)
if(NOT decode_result EQUAL 0)
    message(FATAL_ERROR "baseband decode failed:\n${decode_output}${decode_error}")
endif()
if(NOT EXISTS "${FRAME_PATH}")
    message(FATAL_ERROR "baseband decode produced no PPM:\n${decode_output}${decode_error}")
endif()

execute_process(
    COMMAND "${FIXTURE_TOOL}" validate "${FRAME_PATH}"
    RESULT_VARIABLE validate_result
    OUTPUT_VARIABLE validate_output
    ERROR_VARIABLE validate_error
)
if(NOT validate_result EQUAL 0)
    message(FATAL_ERROR "decoded PPM validation failed:\n${validate_output}${validate_error}")
endif()

function(expect_cli_error label expected)
    execute_process(
        COMMAND "${FAMIDEC}" ${ARGN}
        RESULT_VARIABLE cli_result
        OUTPUT_VARIABLE cli_output
        ERROR_VARIABLE cli_error
    )
    string(CONCAT cli_combined "${cli_output}" "${cli_error}")
    if(NOT cli_result EQUAL 2)
        message(FATAL_ERROR
            "${label}: expected exit 2, got ${cli_result}:\n${cli_combined}")
    endif()
    if(NOT cli_combined MATCHES "${expected}")
        message(FATAL_ERROR
            "${label}: missing '${expected}' diagnostic:\n${cli_combined}")
    endif()
endfunction()

expect_cli_error(
    "missing sample rate" "explicit --rate"
    --input baseband-f32 --file "${FIXTURE_PATH}"
)
expect_cli_error(
    "incomplete fixed AGC" "requires --sync-level and --blank-level"
    --input baseband-f32 --file "${FIXTURE_PATH}" --rate 10000000 --agc fixed
)
expect_cli_error(
    "reversed fixed levels" "sync level greater than blank level"
    --input baseband-f32 --file "${FIXTURE_PATH}" --rate 10000000
    --agc fixed --sync-level 0 --blank-level 1
)
expect_cli_error(
    "non-finite rate" "invalid finite number"
    --input baseband-f32 --file "${FIXTURE_PATH}" --rate nan
)
expect_cli_error(
    "IQ-only spectrum" "only available for IQ input"
    --input baseband-f32 --file "${FIXTURE_PATH}" --rate 10000000 --spectrum
)

message(STATUS "${decode_output}${validate_output}")
file(REMOVE "${FIXTURE_PATH}" "${FRAME_PATH}")
