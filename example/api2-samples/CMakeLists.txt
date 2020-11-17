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


add_definitions(-DFFMPEG_AMIX_MAIN)

set(FILTER_TARGETS
        ffmpeg-amix
        muxing
        transcoding
        transcode_aac
        filter_audio
        )

include_directories(../filters)
include_directories(../filters/erizo)

foreach (target ${FILTER_TARGETS})
    add_executable(${target} ../filters/${target}.cpp)

    find_package(spdlog)
    target_link_libraries(${target}
            PRIVATE spdlog::spdlog_header_only
            avcpp::avcpp
            )

    if (WIN32)
        target_link_libraries(${target} ws2_32)
    endif ()

endforeach ()

