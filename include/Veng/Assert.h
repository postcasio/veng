#pragma once

#include <Veng/Log.h>
#include <stdexcept>
#include <string>

#define VE_ASSERT(condition, ...)                               \
    do {                                                        \
        if (!(condition)) {                                     \
            Veng::Log::Error(__VA_ARGS__);                        \
            throw std::runtime_error(fmt::format(__VA_ARGS__)); \
        }                                                       \
    } while (false)
