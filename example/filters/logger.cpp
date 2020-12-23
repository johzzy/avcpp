//
// Created by Johnny on 2020/11/3.
//

#include "logger.h"
#include <atomic>

#if !defined(SPDLOG_ACTIVE_LEVEL)
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_OFF
#endif

namespace av {

void LogLoader::InitLogger()
{
    spdlog::set_level((spdlog::level::level_enum)SPDLOG_ACTIVE_LEVEL);
    spdlog::set_pattern("[%DT%T%z,%s#%#@%!:%t-%L] %v");

    auto console = spdlog::stdout_logger_mt("console");
    spdlog::set_default_logger(console);

    //        SPDLOG_TRACE("global output with arg {}", 1); // [source main.cpp]
    //        [function main] [line 16] global output with arg 1
    //        SPDLOG_LOGGER_TRACE(console, "logger output with arg {}", 2); //
    //        [source main.cpp] [function main] [line 17] logger output with arg
    //        2 console->info("invoke member function"); // [source ] [function
    //        ] [line ] invoke member function
}
LogLoader::LogLoader()
{
    LoadLogger();
}

void LogLoader::LoadLogger()
{
    static std::atomic_int count{ 0 };
    if (count++ == 0) {
        InitLogger();
    }
}

LogLoader loader;
} // namespace av