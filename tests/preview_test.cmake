# Preview test: with a lossless encoder (FFV1), the "after" frame must be the
# "before" frame exactly, at every time: this checks that the clip is cut and
# its frame found at the right place.
# Usage: cmake -DCONVCLI=<exe> -DMEDIA=<video file> -DOUT_DIR=<dir> -P preview_test.cmake

set(job "${OUT_DIR}/preview_lossless.json")
file(WRITE "${job}" "{ \"input\": \"${MEDIA}\", \"output\": \"${OUT_DIR}/unused.mkv\",
  \"streams\": [ { \"input\": 0, \"action\": \"transcode\", \"encoder\": \"ffv1\" } ] }")

# the start, on and between frames, near the end
foreach(t 0 0.3 2 4.5 6.25 7.77 9.5)
    set(before "${OUT_DIR}/preview_${t}_before.png")
    set(after  "${OUT_DIR}/preview_${t}_after.png")
    file(REMOVE "${before}" "${after}")
    execute_process(COMMAND "${CONVCLI}" preview "${job}" --time ${t} --before "${before}" --after "${after}"
                    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "preview at ${t} s failed:\n${out}${err}")
    endif()
    file(SHA256 "${before}" hb)
    file(SHA256 "${after}" ha)
    if(NOT hb STREQUAL ha)
        message(FATAL_ERROR "at ${t} s the lossless result is not the original frame:\n${out}")
    endif()
    message(STATUS "${t} s: identical")
endforeach()
