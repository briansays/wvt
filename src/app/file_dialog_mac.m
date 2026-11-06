// macOS part of the native file dialogs (file_dialog.cpp). wvt is an X11
// client of XQuartz, not a Cocoa app: its process has no Dock icon and is
// never active, so a Cocoa panel would open behind the XQuartz window and get
// no keys. Before a dialog the process becomes an accessory app (no Dock icon)
// and is activated; afterwards it goes back to no activation at all and the
// app that was in front (XQuartz) gets the focus back. Builds with and
// without ARC.
//
// wvt's main loop reads only the X connection, never the Cocoa event queue.
// An app that is active but does not read that queue leaves macOS waiting on
// it: clicks stop working in every app, not only in wvt, until wvt quits. So
// after a dialog wvt stops being an app that can be active, and the queue is
// read after each dialog and on each frame (wvt_dialog_pump).
#import <AppKit/AppKit.h>

static BOOL g_cocoa = NO;            // a dialog has started Cocoa
static BOOL g_wasProhibited = NO;    // the activation policy before the first dialog

// Hands the Cocoa events in the queue to AppKit (activation changes among
// them) without waiting for more.
static void drainEvents(void) {
    for (;;) {
        NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:[NSDate distantPast]
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
        if (!ev) break;
        [NSApp sendEvent:ev];
    }
}

void* wvt_dialog_begin(void) {
    void* prev = NULL;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        g_cocoa = YES;
        if ([app activationPolicy] == NSApplicationActivationPolicyProhibited) {
            g_wasProhibited = YES;
            [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }
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
    @autoreleasepool {
        NSRunningApplication* prev = previous ? (NSRunningApplication*)CFBridgingRelease(previous) : nil;
        if (prev) {
            if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:prev];
        }
        // No longer an app that can be active: macOS stops waiting on wvt.
        if (g_wasProhibited && ![NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited])
            NSLog(@"wvt: could not leave the accessory activation policy after a file dialog");
        if (prev) [prev activateWithOptions:0];
        drainEvents();
    }
}

void wvt_dialog_pump(void) {
    if (!g_cocoa) return;
    @autoreleasepool {
        drainEvents();
    }
}
