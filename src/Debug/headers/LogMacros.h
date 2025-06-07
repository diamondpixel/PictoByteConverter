#ifndef LOGMACROS_H
#define LOGMACROS_H

#include "LogLevel.h"
#include "LogBufferManager.h"

// Severity numeric levels match comment in LogLevel.h

/**
 * @def LOG_IMPL(LevelConst, Ctx, Category, Message)
 * @brief Helper macro to implement logging for different levels.
 * @param LevelConst The log level constant.
 * @param Ctx The log context.
 * @param Category The module or subsystem name.
 * @param Message The message to log.
 */
#define LOG_IMPL(LevelConst, Ctx, Category, Message)             \
    do {                                                        \
        if constexpr (debug::CurrentLogLevel >= LevelConst) {   \
            debug::LogBufferManager::getInstance().appendTo(    \
                Category, Message, Ctx);                        \
        }                                                       \
    } while (false)

/**
 * @def LOG_ERR(Category, Message)
 * @brief Log an error-level message for the given module.
 * @param Category The module or subsystem name.
 * @param Message The message to log.
 */
#define LOG_ERR(Category, Message)  LOG_IMPL(1, debug::LogContext::Error,   Category, Message)

/**
 * @def LOG_WARN(Category, Message)
 * @brief Log a warning-level message for the given module.
 * @param Category The module or subsystem name.
 * @param Message The message to log.
 */
#define LOG_WARN(Category, Message) LOG_IMPL(2, debug::LogContext::Warning, Category, Message)

/**
 * @def LOG_INF(Category, Message)
 * @brief Log an info-level message for the given module.
 * @param Category The module or subsystem name.
 * @param Message The message to log.
 */
#define LOG_INF(Category, Message)  LOG_IMPL(3, debug::LogContext::Info,    Category, Message)

/**
 * @def LOG_DBG(Category, Message)
 * @brief Log a debug-level message for the given module.
 * @param Category The module or subsystem name.
 * @param Message The message to log.
 */
#define LOG_DBG(Category, Message)  LOG_IMPL(4, debug::LogContext::Debug,   Category, Message)

#endif // LOGMACROS_H
