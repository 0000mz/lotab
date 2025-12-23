#ifndef STATUSBAR_H
#define STATUSBAR_H

// Callback types for menu actions
typedef void (*StatusBarOptionCallback)(void*);

// Context for callbacks and state
typedef struct StatusBarRunContext {
  StatusBarOptionCallback on_toggle;
  StatusBarOptionCallback on_quit;
  void* privdata;
} StatusBarRunContext;

// Initialize and run the Cocoa application loop.
// This function does not return until the application terminates.
// context: Struct containing callbacks and configuration.
void run_daemon_cocoa_app(StatusBarRunContext* context);

// Signals the Cocoa application to terminate (can be called from any thread)
void stop_daemon_cocoa_app(void);

#endif  // STATUSBAR_H
