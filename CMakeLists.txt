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
    src/core/engine.c
    src/core/logcap.c
    src/core/validate.c)
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

# ---- GTK 4 user interface (optional) -------------------------------------------
pkg_check_modules(GTK4 IMPORTED_TARGET gtk4>=4.12)
if(GTK4_FOUND)
    add_executable(ffconv WIN32
        src/ui/main.c
        src/ui/window.c
        src/ui/stream_row.c
        src/ui/option_editor.c
        src/ui/progress.c
        src/ui/uilog.c
        src/ui/ui_util.c)
    target_link_libraries(ffconv PRIVATE convcore PkgConfig::GTK4)
    target_compile_options(ffconv PRIVATE ${CONV_WARNINGS})
else()
    message(STATUS "gtk4 >= 4.12 not found: the ffconv UI will not be built")
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
    # every valid job must also pass validation, dry run included
    add_test(NAME validate_${name} COMMAND convcli validate "${job}"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
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

# GUI tests open real windows: opt in with -DCONV_GUI_TESTS=ON.
option(CONV_GUI_TESTS "Run the ffconv UI tests (they open windows)" OFF)
if(GTK4_FOUND AND CONV_GUI_TESTS)
    add_test(NAME ui_empty
             COMMAND ${CMAKE_COMMAND} -DFFCONV=$<TARGET_FILE:ffconv> -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DMODE=empty -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_test.cmake)
    add_test(NAME ui_open
             COMMAND ${CMAKE_COMMAND} -DFFCONV=$<TARGET_FILE:ffconv> -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DMODE=shot -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/media/m3_in.mkv
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_test.cmake)
    add_test(NAME ui_convert
             COMMAND ${CMAKE_COMMAND} -DFFCONV=$<TARGET_FILE:ffconv> -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DMODE=convert -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/media/m3_in.mkv
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_test.cmake)
    add_test(NAME ui_options
             COMMAND ${CMAKE_COMMAND} -DFFCONV=$<TARGET_FILE:ffconv> -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DMODE=options -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/media/m3_in.mkv
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_test.cmake)
    set_tests_properties(ui_empty ui_open ui_convert ui_options PROPERTIES LABELS gui RUN_SERIAL TRUE)
endif()

# Jobs in tests/jobs/invalid must fail validation with the message in "_expect".
file(GLOB CONV_INVALID_JOBS "${CMAKE_CURRENT_SOURCE_DIR}/tests/jobs/invalid/*.json")
foreach(job ${CONV_INVALID_JOBS})
    get_filename_component(name "${job}" NAME_WE)
    add_test(NAME invalid_${name}
             COMMAND ${CMAKE_COMMAND} -DCONVCLI=$<TARGET_FILE:convcli> -DJOB=${job}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_expect.cmake
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
