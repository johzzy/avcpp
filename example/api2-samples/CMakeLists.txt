cmake_minimum_required(VERSION 3.11)

# transcoder sources
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED yes)

if (NOT DEFINED PROJECT_NAME)
    set(AVCPP_SAMPLES_NOT_SUBPROJECT ON)
endif ()

project(AvCppSamples LANGUAGES CXX VERSION 2.0.99)

if (AVCPP_SAMPLES_NOT_SUBPROJECT)
    find_package(avcpp REQUIRED)
endif ()

set(TARGETS
        api2-decode
        api2-decode-encode-video
        api2-scale-video
        api2-decode-audio
        api2-decode-rasample-audio
        api2-decode-encode-audio
        api2-dict-basic
        api2-timestamp
        api2-demux-seek
        api2-remux
        )

foreach (target ${TARGETS})
    add_executable(${target} ${target}.cpp)

    target_link_libraries(${target}
            avcpp::avcpp
            )

    if (WIN32)
        target_link_libraries(${target} ws2_32)
    endif ()

endforeach ()

#https://ortogonal.github.io/cmake-clang-tidy/
set(CMAKE_CXX_CLANG_TIDY "clang-tidy;-checks=example/*")

find_package(spdlog)
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

add_definitions(-DFFMPEG_AMIX_MAIN)

set(FILTER_TARGETS
        ffmpeg-amix
        muxing
        transcoding
        transcode_aac
        filter_audio
        )

include_directories(../filters)

foreach (target ${FILTER_TARGETS})
    add_executable(${target} ../filters/${target}.cpp)
    target_link_libraries(${target}
            PRIVATE spdlog::spdlog_header_only
            avcpp::avcpp
            )
    if (WIN32)
        target_link_libraries(${target} ws2_32)
    endif ()
endforeach ()

include_directories(../fftools)

set(FFTOOL_TARGETS
        ffmpeg
        ffplay
        ffprobe
        )
set(FFTOOL_COMMON
        ../fftools/cmdutils.cpp
        )

set(ffmpeg_Dependent
        ../fftools/ffmpeg_opt.cpp
        ../fftools/ffmpeg_filter.cpp
        ../fftools/ffmpeg_hw.cpp
        ../fftools/ffmpeg_videotoolbox.cpp
#        ../fftools/ffmpeg_qsv.cpp
        )

set(ffplay_Dependent
        ../fftools/ffplay/common.cpp
        ../fftools/ffplay/VideoState.cpp
        ../fftools/ffplay/Decoder.cpp
        ../fftools/ffplay/FrameQueue.cpp
        ../filters/logger.cpp)

file(GLOB_RECURSE FFTOOL_COMPAT CONFIGURE_DEPENDS ../fftools/*.c)

foreach (target ${FFTOOL_TARGETS})
    add_executable(${target}
            ${${target}_Dependent}
            ../fftools/${target}.cpp
            ${FFTOOL_COMMON}
            ${FFTOOL_COMPAT}
            ${LIBSTDCXX}
            )
    target_link_libraries(${target}
            PRIVATE spdlog::spdlog_header_only
            avcpp::avcpp
            ${SDL2_LIBRARIES}
            "-framework Security"
            "-framework VideoToolbox"
            "-framework CoreServices"
            "-framework Foundation"
            "-framework OpenCL"
            "-framework CoreMedia"
            "-framework AudioToolbox"
            "-framework CoreVideo"
            "-framework AVFoundation"
            "-framework CoreFoundation"
            "-framework CoreGraphics"
            "-framework Cocoa"
            "-framework Accelerate"
            )

    if (WIN32)
        target_link_libraries(${target} ws2_32)
    endif ()

    message(STATUS "${target}: ${${target}_Dependent}")
endforeach ()

message(STATUS "compat: ${FFTOOL_COMPAT}")

if (0)

set(ffplay "ffplay.cc")
set(ffplay_Dependent
        "ffplay/common.xxx")

set(item "ffplay")
set(item "ffmpeg")

message(STATUS "item ${item}")
message(STATUS "item ${${item}_Dependent}")

endif ()