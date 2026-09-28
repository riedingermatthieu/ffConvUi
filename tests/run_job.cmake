# End-to-end test: job-template -> run -> probe the result.
# Usage: cmake -DCONVCLI=<exe> -DMEDIA=<input> -DOUT_DIR=<dir> -DEXT=<mp4|mkv|...> -P run_job.cmake

set(job "${OUT_DIR}/e2e_${EXT}.json")
set(out "${OUT_DIR}/e2e_out.${EXT}")
file(REMOVE "${out}")

execute_process(COMMAND "${CONVCLI}" job-template "${MEDIA}" --output "${out}"
                OUTPUT_FILE "${job}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "job-template failed (${rc})")
endif()

execute_process(COMMAND "${CONVCLI}" run "${job}" --overwrite --quiet RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "run failed (${rc})")
endif()

execute_process(COMMAND "${CONVCLI}" probe "${out}" OUTPUT_VARIABLE probe RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT probe MATCHES "Stream #0")
    message(FATAL_ERROR "output is not readable:\n${probe}")
endif()
message(STATUS "${probe}")
