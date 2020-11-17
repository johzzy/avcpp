//
// Created by Johnny on 2020/11/3.
//

#pragma once

// Enable SPDLOG_TRACE macro, see: spdlog/tweakme.h
// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
// #define SPDLOG_DEBUG_ON 1
// #define SPDLOG_TRACE_ON 1

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace av {

class LogLoader {
    void InitLogger();
    void LoadLogger();

public:
    LogLoader();
};

} // namespace bot
