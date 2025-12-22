#ifndef STATUSBAR_H
#define STATUSBAR_H

// Callback types for menu actions
typedef void (*StatusBarOptionCallback)(void);

// Initialize and run the Cocoa application loop.
// This function does not return until the application terminates.
// on_toggle: Callback for Hotkey trigger
// on_quit: Callback for "Quit" menu item
void run_daemon_cocoa_app(StatusBarOptionCallback on_toggle,
                          StatusBarOptionCallback on_quit);

#endif // STATUSBAR_H
