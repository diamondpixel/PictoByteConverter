#ifndef LOGMACROS_H
#define LOGMACROS_H

#include "LogLevel.h"
#include "LogBufferManager.h"

// Severity numeric levels match comment in LogLevel.h
#define LOG_IMPL(LevelConst, Ctx, Category, Message)             \
    do {                                                        \
        if constexpr (debug::CurrentLogLevel >= LevelConst) {   \
            debug::LogBufferManager::getInstance().appendTo(    \
                Category, Message, Ctx);                        \
        }                                                       \
    } while (false)

#define LOG_ERR(Category, Message)  LOG_IMPL(1, debug::LogContext::Error,   Category, Message)
#define LOG_WARN(Category, Message) LOG_IMPL(2, debug::LogContext::Warning, Category, Message)
#define LOG_INF(Category, Message)  LOG_IMPL(3, debug::LogContext::Info,    Category, Message)
#define LOG_DBG(Category, Message)  LOG_IMPL(4, debug::LogContext::Debug,   Category, Message)

#endif // LOGMACROS_H
