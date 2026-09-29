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
    src/core/validate.c
    src/core/cmdline.c
    src/core/fsutil.c
    src/core/preview.c)
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
        src/ui/choice_item.c
        src/ui/preview_window.c
        src/ui/option_editor.c
        src/ui/progress.c
        src/ui/uilog.c
        src/ui/ui_util.c)
    target_link_libraries(ffconv PRIVATE convcore PkgConfig::GTK4)

    # The UI test driver (FFCONV_TEST_* variables) lives in its own file and is
    # only compiled in on request; the application code has no test code.
    option(CONV_TEST_HOOKS "Build the UI test driver into ffconv (needed by the UI tests)" ON)
    if(CONV_TEST_HOOKS)
        target_sources(ffconv PRIVATE src/ui/testhooks.c)
    else()
        target_sources(ffconv PRIVATE src/ui/testhooks_none.c)
    endif()
    target_compile_options(ffconv PRIVATE ${CONV_WARNINGS})

    # App icon for GTK windows: compiled into the program as a GResource
    # (regenerate the icon files with tools/make_icons.py)
    find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
    set(CONV_GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ffconv.gresource.xml)
    set(CONV_GRESOURCE_C   ${CMAKE_CURRENT_BINARY_DIR}/ffconv-resources.c)
    execute_process(COMMAND ${GLIB_COMPILE_RESOURCES} --generate-dependencies
                            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR} ${CONV_GRESOURCE_XML}
                    OUTPUT_VARIABLE CONV_GRESOURCE_DEPS OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "\n" ";" CONV_GRESOURCE_DEPS "${CONV_GRESOURCE_DEPS}")
    add_custom_command(OUTPUT ${CONV_GRESOURCE_C}
        COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --target=${CONV_GRESOURCE_C}
                --sourcedir=${CMAKE_CURRENT_SOURCE_DIR} ${CONV_GRESOURCE_XML}
        DEPENDS ${CONV_GRESOURCE_XML} ${CONV_GRESOURCE_DEPS}
        COMMENT "Compiling UI resources")
    target_sources(ffconv PRIVATE ${CONV_GRESOURCE_C})

    # Windows: icon shown by Explorer, shortcuts and the taskbar, and version info
    if(WIN32)
        enable_language(RC)
        set(CONV_ICO ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/ffconv.ico)
        configure_file(src/ui/ffconv.rc.in ${CMAKE_CURRENT_BINARY_DIR}/ffconv.rc @ONLY)
        target_sources(ffconv PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ffconv.rc)
        set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/ffconv.rc PROPERTIES OBJECT_DEPENDS ${CONV_ICO})
    endif()
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

# Preview: a lossless encoder must give back the original frame exactly.
add_test(NAME preview_lossless
         COMMAND ${CMAKE_COMMAND} -DCONVCLI=$<TARGET_FILE:convcli>
                 -DMEDIA=${CMAKE_CURRENT_SOURCE_DIR}/tests/media/m3_in.mkv
                 -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/preview_test.cmake)

# The equivalent ffmpeg command must give the engine's result (needs the
# ffmpeg CLI and bash, both in MSYS2).
find_program(CONV_FFMPEG_CLI ffmpeg)
find_program(CONV_BASH bash)
if(CONV_FFMPEG_CLI AND CONV_BASH)
    # trim: only the video is compared. After the seek, audio timestamps
    # settle ~2 ms apart (the source's AC3 stores milliseconds), and ffmpeg
    # also copies subtitle events that end before -ss.
    foreach(spec "m3_acceptance:v;a;s" "drawtext_escaping:v;a;s" "trim:v")
        string(REPLACE ":" ";" parts "${spec}")
        list(GET parts 0 name)
        list(REMOVE_AT parts 0)
        string(REPLACE ";" "," types "${parts}")
        add_test(NAME command_${name}
                 COMMAND ${CMAKE_COMMAND} -DCONVCLI=$<TARGET_FILE:convcli> -DFFMPEG=${CONV_FFMPEG_CLI}
                         -DBASH=${CONV_BASH} -DJOB=${CMAKE_CURRENT_SOURCE_DIR}/tests/jobs/${name}.json
                         -DTYPES=${types} -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/command_test.cmake
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endforeach()
endif()

# GUI tests open real windows: opt in with -DCONV_GUI_TESTS=ON.
option(CONV_GUI_TESTS "Run the ffconv UI tests (they open windows)" OFF)
if(GTK4_FOUND AND CONV_GUI_TESTS AND NOT CONV_TEST_HOOKS)
    message(WARNING "CONV_GUI_TESTS needs CONV_TEST_HOOKS=ON (the UI test driver): UI tests skipped")
endif()
if(GTK4_FOUND AND CONV_GUI_TESTS AND CONV_TEST_HOOKS)
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
    foreach(mode greyed fixed)
        add_test(NAME ui_${mode}
                 COMMAND ${CMAKE_COMMAND} -DFFCONV=$<TARGET_FILE:ffconv> -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                         -DMODE=${mode} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test.mkv
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_test.cmake)
    endforeach()
    add_test(NAME ui_preview
             COMMAND ${CMAKE_COMMAND} -DFFCONV=$<TARGET_FILE:ffconv> -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DMODE=preview -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/media/m3_in.mkv
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_test.cmake)
    set_tests_properties(ui_empty ui_open ui_convert ui_options ui_greyed ui_fixed ui_preview
                         PROPERTIES LABELS gui RUN_SERIAL TRUE)
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
