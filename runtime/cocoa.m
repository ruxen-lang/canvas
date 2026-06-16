// AppKit widget backend for quiver (macOS). The ElementSurface reconciler drives
// these cocoa_* ops (exposed in Ruxen as the `MacOS` class) to create + lay out
// REAL NSView widgets — the OS owns text editing, caret, scrolling, focus, IME.
//
// Event model: a POLLING loop (no C->Ruxen callbacks). quiver calls `cocoa_pump`
// in a loop; each call processes one NSEvent and returns the node id of a control
// that fired (button/checkbox), -1 for nothing, or -2 when the window closed.
// quiver then runs that node's handler, flushes, and patches the dirty widgets.
#import <Cocoa/Cocoa.h>

#define COCOA_MAX_NODES 8192
static NSView *g_views[COCOA_MAX_NODES];
static NSWindow *g_window = nil;
static NSView *g_content = nil;
static long g_pending = -1;   // node id of the control that fired this pump, or -1
static int g_closed = 0;      // set by the window delegate on close

// Top-left-origin content view so quiver's (x, y) map directly to AppKit frames.
@interface QuiverFlippedView : NSView
@end
@implementation QuiverFlippedView
- (BOOL)isFlipped { return YES; }
@end

// Target for control actions: stash the firing control's node id (its .tag) so
// the next cocoa_pump hands it back to quiver. No re-entrancy into Ruxen.
@interface QuiverTarget : NSObject
- (void)onAction:(id)sender;
@end
@implementation QuiverTarget
- (void)onAction:(id)sender {
  g_pending = [sender tag];
}
@end

@interface QuiverWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation QuiverWindowDelegate
- (void)windowWillClose:(NSNotification *)n { g_closed = 1; }
@end

static QuiverTarget *g_target = nil;

static NSView *node_view(long id) {
  if (id < 0 || id >= COCOA_MAX_NODES) return nil;
  return g_views[id];
}

void cocoa_window_open(const char *title, long w, long h) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    g_target = [[QuiverTarget alloc] init];   // owned for the app's lifetime

    NSRect frame = NSMakeRect(0, 0, (CGFloat)w, (CGFloat)h);
    g_window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [g_window setTitle:[NSString stringWithUTF8String:title]];
    [g_window setDelegate:[[QuiverWindowDelegate alloc] init]];
    [g_window setReleasedWhenClosed:NO];
    [g_window center];

    g_content = [[QuiverFlippedView alloc] initWithFrame:frame];
    [g_window setContentView:g_content];

    // Finish launching so the app is initialized BEFORE quiver mounts widgets.
    [NSApp finishLaunching];
  }
}

void cocoa_create(long id, long kind) {
  if (id < 0 || id >= COCOA_MAX_NODES) return;
  @autoreleasepool {
    NSView *v = nil;
    switch (kind) {
      case 1:   // text
      case 2: { // dyn_text
        v = [NSTextField labelWithString:@""];
        break;
      }
      case 3: { // button
        NSButton *b = [NSButton buttonWithTitle:@"" target:g_target action:@selector(onAction:)];
        [b setBezelStyle:NSBezelStyleRounded];
        b.tag = id;
        v = b;
        break;
      }
      case 7: { // text input
        NSTextField *t = [NSTextField textFieldWithString:@""];
        t.tag = id;
        v = t;
        break;
      }
      case 8: { // checkbox
        NSButton *c = [NSButton checkboxWithTitle:@"" target:g_target action:@selector(onAction:)];
        c.tag = id;
        v = c;
        break;
      }
      case 9: { // slider
        NSSlider *s = [NSSlider sliderWithValue:0 minValue:0 maxValue:100 target:g_target action:@selector(onAction:)];
        s.tag = id;
        v = s;
        break;
      }
      case 10: { // select
        NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        p.target = g_target;
        p.action = @selector(onAction:);
        p.tag = id;
        v = p;
        break;
      }
      default: { // containers -> a plain flipped view
        v = [[QuiverFlippedView alloc] initWithFrame:NSZeroRect];
        break;
      }
    }
    // MRC (no ARC): the factory builders return AUTORELEASED views; retain so
    // they survive this @autoreleasepool drain (else g_views[id] dangles and the
    // next set_frame:/set_text: crashes in objc_msgSend).
    [v retain];
    g_views[id] = v;
  }
}

void cocoa_set_text(long id, const char *text) {
  NSView *v = node_view(id);
  if (!v || !text) return;
  NSString *s = [NSString stringWithUTF8String:text];
  if (!s) return;  // invalid UTF-8 -> nil; setStringValue:/setTitle: would throw
  if ([v isKindOfClass:[NSTextField class]]) {
    [(NSTextField *)v setStringValue:s];
  } else if ([v isKindOfClass:[NSButton class]]) {
    [(NSButton *)v setTitle:s];
  }
}

void cocoa_set_frame(long id, long x, long y, long w, long h) {
  NSView *v = node_view(id);
  if (!v) return;
  [v setFrame:NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h)];
}

void cocoa_set_state(long id, long value) {
  NSView *v = node_view(id);
  if (!v) return;
  // NSPopUpButton is a subclass of NSButton — test it first.
  if ([v isKindOfClass:[NSPopUpButton class]]) {
    [(NSPopUpButton *)v selectItemAtIndex:value];
  } else if ([v isKindOfClass:[NSSlider class]]) {
    [(NSSlider *)v setDoubleValue:(double)value];
  } else if ([v isKindOfClass:[NSButton class]]) {
    [(NSButton *)v setState:(value != 0 ? NSControlStateValueOn : NSControlStateValueOff)];
  }
}

void cocoa_add_subview(long parent, long child) {
  NSView *c = node_view(child);
  if (!c) return;
  NSView *p = (parent == -1) ? g_content : node_view(parent);
  if (p) [p addSubview:c];
}

// Show the window + bring the app forward. Call AFTER quiver mounts the tree.
void cocoa_show(void) {
  [g_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

// Process ONE event and report what fired. Blocks until an event arrives (so the
// loop is idle-cheap). A control action sets g_pending via QuiverTarget; the
// window delegate sets g_closed. Returns: node id (>=0) that fired, -2 if the
// window closed, else -1.
long cocoa_pump(void) {
  g_pending = -1;
  @autoreleasepool {
    NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                     untilDate:[NSDate distantFuture]
                                        inMode:NSDefaultRunLoopMode
                                       dequeue:YES];
    if (ev) [NSApp sendEvent:ev];
  }
  if (g_closed) return -2;
  return g_pending;
}
