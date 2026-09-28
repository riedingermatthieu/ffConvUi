cmake_minimum_required(VERSION 3.20)
project(ffconv VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# FFmpeg >= 7.1 (needed for avcodec_get_supported_config, AV_OPT_TYPE_UINT,
# AV_OPT_TYPE_FLAG_ARRAY and the AVChannelLayout API).
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat>=61.7.100
    libavcodec>=61.19.100
    libavfilter>=10.4.100
    libavutil>=59.39.100
    libswscale
    libswresample)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CONV_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wshadow -Wstrict-prototypes)
elseif(MSVC)
    set(CONV_WARNINGS /W4)
endif()

# ---- core library (no UI dependencies) -------------------------------------
add_library(convcore STATIC
    src/core/probe.c
    src/core/caps.c
    src/core/avopt_schema.c
    src/core/json.c
    src/core/job.c
    src/core/engine.c)
target_include_directories(convcore PUBLIC src/core)
target_link_libraries(convcore PUBLIC PkgConfig::FFMPEG)
target_compile_options(convcore PRIVATE ${CONV_WARNINGS})

# ---- headless CLI ------------------------------------------------------------
add_executable(convcli src/cli/convcli.c)
target_link_libraries(convcli PRIVATE convcore)
target_compile_options(convcli PRIVATE ${CONV_WARNINGS})
if(WIN32)
    target_link_libraries(convcli PRIVATE shell32)
endif()

# ---- smoke tests -------------------------------------------------------------
enable_testing()
add_test(NAME version        COMMAND convcli version)
add_test(NAME muxers         COMMAND convcli muxers --common)
add_test(NAME encoders_video COMMAND convcli encoders --type video --common)
add_test(NAME filters_video  COMMAND convcli filters --type video)
add_test(NAME options_scale  COMMAND convcli options filter scale)
add_test(NAME options_mp4    COMMAND convcli options muxer mp4)
set(CONV_TEST_MEDIA "${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test.mkv" CACHE FILEPATH
    "Media file used by the probe/actions/e2e tests (empty to skip them)")
if(CONV_TEST_MEDIA AND EXISTS "${CONV_TEST_MEDIA}")
    add_test(NAME probe       COMMAND convcli probe ${CONV_TEST_MEDIA})
    add_test(NAME muxers_for  COMMAND convcli muxers --for ${CONV_TEST_MEDIA} --common)
    add_test(NAME actions_mp4 COMMAND convcli actions ${CONV_TEST_MEDIA} --muxer mp4)
    foreach(ext mp4 mkv webm)
        add_test(NAME e2e_${ext}
                 COMMAND ${CMAKE_COMMAND} -DCONVCLI=$<TARGET_FILE:convcli> -DMEDIA=${CONV_TEST_MEDIA}
                         -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR} -DEXT=${ext}
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_job.cmake)
    endforeach()
endif()

# Job files in tests/jobs use paths relative to the project root
# (inputs in tests/media, outputs in build/).
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")
file(GLOB CONV_TEST_JOBS "${CMAKE_CURRENT_SOURCE_DIR}/tests/jobs/*.json")
foreach(job ${CONV_TEST_JOBS})
    get_filename_component(name "${job}" NAME_WE)
    if(name MATCHES "^cancel")
        add_test(NAME job_${name} COMMAND convcli run "${job}" --overwrite --quiet --cancel-after 1
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        # a cancelled run exits with 1; pass on the message instead of the exit code
        set_tests_properties(job_${name} PROPERTIES PASS_REGULAR_EXPRESSION "Cancelled")
    else()
        add_test(NAME job_${name} COMMAND convcli run "${job}" --overwrite --quiet
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endforeach()
