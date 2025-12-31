#include "statusbar.h"
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

// Forward declaration
OSStatus HotKeyHandler(EventHandlerCallRef nextHandler, EventRef theEvent, void* userData);

@interface StatusBarDelegate : NSObject <NSApplicationDelegate>
@property(strong, nonatomic) NSStatusItem* statusItem;
@property(nonatomic, assign) StatusBarRunContext* context;
@end

@implementation StatusBarDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)aNotification {
  // Determine activation policy
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

  // Create Status Item
  // NSVariableStatusItemLength is the correct constant for variable length
  // status items
  self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];

  // Set Icon
  if (self.statusItem.button) {
    if (@available(macOS 11.0, *)) {
      self.statusItem.button.image = [NSImage imageWithSystemSymbolName:@"rectangle.stack"
                                               accessibilityDescription:@"Tab Manager"];
    } else {
      self.statusItem.button.title = @"TM";
    }
  }

  // Create Menu
  NSMenu* menu = [[NSMenu alloc] init];

  NSMenuItem* headerItem = [[NSMenuItem alloc] initWithTitle:@"Tab Manager" action:nil keyEquivalent:@""];
  [headerItem setEnabled:NO];
  [menu addItem:headerItem];

  [menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit" action:@selector(quitAction) keyEquivalent:@"q"];
  [quitItem setTarget:self];
  [menu addItem:quitItem];

  self.statusItem.menu = menu;

  // Register Global Hotkey
  EventHotKeyRef gMyHotKeyRef;
  EventHotKeyID gMyHotKeyID;
  EventTypeSpec eventType;
  eventType.eventClass = kEventClassKeyboard;
  eventType.eventKind = kEventHotKeyPressed;

  InstallApplicationEventHandler(NewEventHandlerUPP(HotKeyHandler), 1, &eventType, (__bridge void*)self, NULL);

  gMyHotKeyID.signature = 'htk1';
  gMyHotKeyID.id = 1;

  UInt32 keyCode = 38;  // Default J
  UInt32 modifiers = cmdKey | shiftKey;

  if (self.context && self.context->keybind) {
    NSString* bind = [NSString stringWithUTF8String:self.context->keybind];
    NSArray* parts = [bind componentsSeparatedByString:@"+"];
    modifiers = 0;
    for (NSString* part in parts) {
      NSString* p = [part uppercaseString];
      if ([p isEqualToString:@"CMD"] || [p isEqualToString:@"COMMAND"])
        modifiers |= cmdKey;
      else if ([p isEqualToString:@"CTRL"] || [p isEqualToString:@"CONTROL"])
        modifiers |= controlKey;  // Carbon ctrl is controlKey
      else if ([p isEqualToString:@"ALT"] || [p isEqualToString:@"OPTION"])
        modifiers |= optionKey;
      else if ([p isEqualToString:@"SHIFT"])
        modifiers |= shiftKey;
      else {
        // Simple char mapping
        if ([p isEqualToString:@"J"])
          keyCode = 38;
        else if ([p isEqualToString:@"K"])
          keyCode = 40;
        else if ([p isEqualToString:@"L"])
          keyCode = 37;
        else if ([p isEqualToString:@"SPACE"])
          keyCode = 49;
        // Add more as needed or use a robust valid mapping
        // Fallback for demo
      }
    }
  }

  RegisterEventHotKey(keyCode, modifiers, gMyHotKeyID, GetApplicationEventTarget(), 0, &gMyHotKeyRef);
}

- (void)toggleAction {
  if (self.context->on_toggle) {
    self.context->on_toggle(self.context->privdata);
  }
}

- (void)quitAction {
  if (self.context->on_quit) {
    self.context->on_quit(self.context->privdata);  // Signal main loop to exit if possible, or perform cleanup
  }
  [NSApp terminate:nil];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  // Optional: Log or finalize hooks
  // OS cleans up memory/sockets on exit
}

@end

// Global C-function callback for HotKeys
OSStatus HotKeyHandler(EventHandlerCallRef nextHandler, EventRef theEvent, void* userData) {
  (void)nextHandler;
  EventHotKeyID hkID;
  GetEventParameter(theEvent, kEventParamDirectObject, typeEventHotKeyID, NULL, sizeof(hkID), NULL, &hkID);

  if (hkID.id == 1) {
    StatusBarDelegate* delegate = (__bridge StatusBarDelegate*)userData;
    [delegate toggleAction];
  }
  return noErr;
}

void run_daemon_cocoa_app(StatusBarRunContext* context) {
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    StatusBarDelegate* delegate = [[StatusBarDelegate alloc] init];
    delegate.context = context;
    app.delegate = delegate;
    [app run];
  }
}

void stop_daemon_cocoa_app(void) {
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp terminate:nil];
  });
}
