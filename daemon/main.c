#include <argparse.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "engine.h"

EngineContext* ectx = NULL;

// --- Adapter Implementations ---
void sigint_handler(int sig) {
  (void)sig;
  printf("Daemon: Caught SIGINT\n");
}

int main(int argc, const char** argv) {
  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  // Arg Parse
  const char* loglevel_str = NULL;
  const char* app_path = NULL;
  const char* daemon_manifest_path = NULL;
  const char* gui_manifest_path = NULL;
  const char* allowed_browser_id = NULL;

  struct argparse_option options[] = {
      OPT_HELP(),
      OPT_STRING('l', "loglevel", &loglevel_str, "info(default)|trace", NULL, 0, 0),
      OPT_STRING('a', "app-path", &app_path, "Path to the Lotab.app or executable", NULL, 0, 0),
      OPT_STRING('d', "daemon-manifest-path", &daemon_manifest_path, "Path to dump daemon manifest", NULL, 0, 0),
      OPT_STRING('g', "gui-manifest-path", &gui_manifest_path, "Path to dump GUI manifest", NULL, 0, 0),
      OPT_STRING(0, "allowed-browser-id", &allowed_browser_id, "Restrict to specific browser ID", NULL, 0, 0),
      OPT_END(),
  };

  struct argparse argparse;
  static const char* const usages[] = {
      "daemon [options]",
      NULL,
  };

  argparse_init(&argparse, options, usages, 0);
  argparse_describe(&argparse, "\nTabManager Daemon", "\nControls the TabManager backend.");
  argc = argparse_parse(&argparse, argc, argv);

  LogLevel loglevel = LOG_LEVEL_INFO;
  if (loglevel_str && strcmp(loglevel_str, "trace") == 0) {
    loglevel = LOG_LEVEL_TRACE;
  }
  engine_set_log_level(loglevel);

  // Init Engine
  EngineCreationInfo create_info = {
      .port = 9001,
      .enable_statusbar = 1,
      .app_path = app_path,
      .daemon_manifest_path = daemon_manifest_path,
      .gui_manifest_path = gui_manifest_path,
      .allowed_browser_id = allowed_browser_id,
  };
  if (engine_init(&ectx, create_info) != 0) {
    fprintf(stderr, "Failed to initialize engine.\n");
    return 1;
  }
  engine_run(ectx);
  printf("Daemon: Exiting\n");
  if (ectx != NULL) {
    engine_destroy(ectx);
  }
  free(ectx);
  return 0;
}
