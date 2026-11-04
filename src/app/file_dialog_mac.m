// macOS part of the native file dialogs (file_dialog.cpp). wvt is an X11
// client of XQuartz, not a Cocoa app: its process has no Dock icon and is
// never active, so a Cocoa panel would open behind the XQuartz window and get
// no keys. Before a dialog the process becomes an accessory app (no Dock icon)
// and is activated; afterwards the app that was in front (XQuartz) gets the
// focus back. Builds with and without ARC.
#import <AppKit/AppKit.h>

void* wvt_dialog_begin(void) {
    void* prev = NULL;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        if ([app activationPolicy] == NSApplicationActivationPolicyProhibited)
            [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
        if (front && ![front isEqual:[NSRunningApplication currentApplication]]) prev = (void*)CFBridgingRetain(front);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        // Deprecated in macOS 14 for "cooperative" activation, which needs
        // the app in front to yield; XQuartz does not know to.
        [app activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
    return prev;
}

void wvt_dialog_end(void* previous) {
    if (!previous) return;
    @autoreleasepool {
        NSRunningApplication* prev = (NSRunningApplication*)CFBridgingRelease(previous);
        if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:prev];
        [prev activateWithOptions:0];
    }
}
