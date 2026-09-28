# Validation test: `convcli validate` must fail and its output must match the
# regular expression in the job's "_expect" field.
# Usage: cmake -DCONVCLI=<exe> -DJOB=<job.json> -P validate_expect.cmake
# (run from the project root: job paths are relative to it)

file(READ "${JOB}" text)
string(JSON expect ERROR_VARIABLE json_error GET "${text}" "_expect")
if(json_error)
    message(FATAL_ERROR "${JOB}: no \"_expect\" field (${json_error})")
endif()

execute_process(COMMAND "${CONVCLI}" validate "${JOB}"
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
message(STATUS "${out}")
if(rc EQUAL 0)
    message(FATAL_ERROR "validation passed but should have failed")
endif()
if(NOT out MATCHES "${expect}")
    message(FATAL_ERROR "output does not match \"${expect}\"")
endif()
