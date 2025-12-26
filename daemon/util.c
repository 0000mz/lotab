#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static LogLevel g_log_level = LOG_LEVEL_INFO;

char log_level_str(LogLevel level) {
  switch (level) {
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
  char buf[2048];
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
  uint64_t tid;
  pthread_threadid_np(NULL, &tid);
  fprintf(f, "%s%c %d:%llu [%s @ %p]%s %s", color, l_prefix, getpid(), tid, ecls ? ecls->name : "null", (void*)ecls,
          reset, buf);
}

void vlog_s(int level, struct EngClass* cls, const char* msg) {
  vlog((LogLevel)level, &cls, "%s\n", msg);
}

void engine_set_log_level(int level) {
  g_log_level = (LogLevel)level;
}

int engine_get_log_level(void) {
  return (int)g_log_level;
}
