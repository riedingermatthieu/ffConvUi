# The equivalent ffmpeg command must produce the same result as the engine.
# Usage: cmake -DCONVCLI=<exe> -DFFMPEG=<ffmpeg> -DBASH=<bash> -DJOB=<job.json> -DOUT_DIR=<dir>
#              -P command_test.cmake        (run from the project root)
#
# Runs the job with convcli, runs `convcli command --shell bash` with the real
# ffmpeg, and compares the decoded frames (framemd5: data and timestamps).

get_filename_component(name "${JOB}" NAME_WE)
file(READ "${JOB}" job)
string(JSON output GET "${job}" output)
get_filename_component(ext "${output}" LAST_EXT)

foreach(v engine cli)
    set(out_${v} "${OUT_DIR}/cmdtest_${name}_${v}${ext}")
    file(REMOVE "${out_${v}}")
    string(JSON job_${v} SET "${job}" output "\"${out_${v}}\"")
    string(JSON job_${v} SET "${job_${v}}" overwrite "true")
    file(WRITE "${OUT_DIR}/cmdtest_${name}_${v}.json" "${job_${v}}")
endforeach()

execute_process(COMMAND "${CONVCLI}" run "${OUT_DIR}/cmdtest_${name}_engine.json" --quiet
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the engine failed (${rc})")
endif()

execute_process(COMMAND "${CONVCLI}" command "${OUT_DIR}/cmdtest_${name}_cli.json" --shell bash
                OUTPUT_VARIABLE cmd OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "convcli command failed")
endif()
message(STATUS "${cmd}")
string(REGEX REPLACE "^ffmpeg " "\"${FFMPEG}\" -v error -nostdin " cmd "${cmd}")
# through a script file: passing it as `bash -c <command>` would go through
# the Windows command line, which MSYS bash re-parses with its own rules
file(WRITE "${OUT_DIR}/cmdtest_${name}.sh" "${cmd}\n")
execute_process(COMMAND "${BASH}" "${OUT_DIR}/cmdtest_${name}.sh" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the ffmpeg command failed (${rc})")
endif()

foreach(type v a s)
    foreach(v engine cli)
        execute_process(COMMAND "${FFMPEG}" -v error -i "${out_${v}}" -map 0:${type}? -f framemd5 -
                        OUTPUT_VARIABLE md5_${v} ERROR_QUIET)
        string(REGEX REPLACE "#[^\n]*\n" "" md5_${v} "${md5_${v}}")   # drop the header comments
    endforeach()
    if(NOT md5_engine STREQUAL md5_cli)
        message(FATAL_ERROR "the '${type}' streams differ between the engine and the ffmpeg command")
    endif()
endforeach()
