#pragma once

#ifndef DAEMON_UTIL_H_
#define DAEMON_UTIL_H_

#ifdef __cplusplus
extern "C" {
#endif

struct EngClass {
  char* name;
};

#if defined(SWIFT_BRIDGE)
#include <CoreFoundation/CoreFoundation.h>
typedef CF_ENUM(int, NsLogLevel) { NsLogLevelWarn = 0, NsLogLevelError = 1, NsLogLevelInfo = 2, NsLogLevelTrace = 3 };
void vlog_s(NsLogLevel level, struct EngClass* cls, const char* msg);
#endif
typedef enum { LOG_LEVEL_WARN = 0, LOG_LEVEL_ERROR = 1, LOG_LEVEL_INFO = 2, LOG_LEVEL_TRACE = 3 } LogLevel;

// Log a message with the specified level
void vlog(LogLevel level, void* cls, const char* fmt, ...);
void engine_set_log_level(LogLevel level);

#ifdef __cplusplus
}
#endif

#endif  // DAEMON_UTIL_H_
