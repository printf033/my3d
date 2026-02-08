#pragma once

enum class LEVEL
{
    TRACE = 0,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

#include <functional>
#include <string>

using OutputFunction = std::function<void(std::string_view msg)>;

using FlushFunction = std::function<void(void)>;

#include <iostream>

struct OptionsLogger
{
    LEVEL suppressLevel = LEVEL::TRACE;
    const char *const *lv2str = LV2STR_vivid.data();
    OutputFunction outputFunction = [](std::string_view msg)
    { std::cout << msg; };
    FlushFunction flushFunction = []()
    { std::cout.flush(); };

    static constexpr std::array<const char *, 6> LV2STR =
        {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    static constexpr std::array<const char *, 6> LV2STR_vivid =
        {"\033[7;37mTRACE\033[0m",
         "\033[7;36mDEBUG\033[0m",
         "\033[7;32mINFO \033[0m",
         "\033[7;33mWARN \033[0m",
         "\033[7;31mERROR\033[0m",
         "\033[5;41mFATAL\033[0m"};
};

#include <chrono>

struct OptionsFileManager
{
    size_t fileBufferCapacity = 2097152UL; // 2M
    size_t fileCapacity = 104857600UL;     // 100M
    std::string basename = "mylog";
    std::chrono::seconds intervalFlushFile{30};
};

struct OptionsMultiBuffer
{
    size_t preBufferCapacity = 1048576UL; // 1M
    std::chrono::seconds intervalFlushBuffer{5};
};
