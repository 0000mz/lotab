#include "util.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/_types/_va_list.h>

static pthread_mutex_t g_log_mu;
static LogLevel g_log_level = LOG_LEVEL_INFO;

char log_level_str(const LogLevel l) {
  switch (l) {
    case LOG_LEVEL_INFO:
      return 'I';
    case LOG_LEVEL_TRACE:
      return 'T';
    case LOG_LEVEL_WARN:
      return 'W';
    case LOG_LEVEL_ERROR:
      return 'E';
  }
}

void vlog(LogLevel level, void* cls, const char* fmt, ...) {
  if (level > g_log_level) {
    return;
  }
  char buf[1 << 11];  // 2048
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  FILE* f = stdout;
  if (level == LOG_LEVEL_ERROR)
    f = stderr;

  struct EngClass* ecls = cls == NULL ? NULL : *(struct EngClass**)cls;
  char l_prefix = log_level_str(level);

  const char* color = "";
  const char* reset = "\033[0m";
  switch (level) {
    case LOG_LEVEL_WARN:
      color = "\033[0;33m";  // Yellow
      break;
    case LOG_LEVEL_ERROR:
      color = "\033[0;31m";  // Red
      break;
    case LOG_LEVEL_INFO:
      color = "\033[0;36m";  // Cyan
      break;
    case LOG_LEVEL_TRACE:
      color = "\033[0;35m";  // Purple
      break;
    default:
      break;
  }
  pthread_mutex_lock(&g_log_mu);
  fprintf(f, "%s%c [%s @ %p]%s %s", color, l_prefix, ecls ? ecls->name : "null", (void*)ecls, reset, buf);
  pthread_mutex_unlock(&g_log_mu);
}

void engine_set_log_level(LogLevel level) {
  g_log_level = level;
}
