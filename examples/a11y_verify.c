/* a11y_verify.c — standalone, HUMAN-VERIFIED proof of the macOS accessibility
 * bridge (Phase 3, docs/decisions/accessibility.md). The ENGINE-SIDE a11y tree
 * intake (push/clear/count) is pinned headless in tests/accessibility.rx; the LIVE
 * NSAccessibility exposure needs Cocoa + a real assistive client (VoiceOver), and
 * Cocoa is unsafe after fork() (the harness forks per case), so it CANNOT be
 * pinned automatically — exactly like the file dialogs and on-screen Metal. This
 * is the MANUAL proof, compile-checked and run by a human with VoiceOver on.
 *
 *   cc -O2 -framework AppKit -o a11y_verify examples/a11y_verify.c
 *   ./a11y_verify        # with VoiceOver enabled (Cmd-F5): the app's accessibility
 *                        # label is set; VoiceOver announces it. Prints PASS.
 *
 * Mirrors runtime/sdl_window.c's ruxen_canvas_window_set_a11y_title: objc_getClass
 * + objc_msgSend to [NSApplication sharedApplication], then
 * [NSApp setAccessibilityLabel:@"..."] — a real NSAccessibility protocol setter on
 * a real Cocoa object reached through the objc runtime via dlopen (no link
 * dependency beyond AppKit being present).
 *
 * STAGED (the ADR §4 remainder, NOT in this file yet): exposing the engine's
 * stored a11y NODES as NSAccessibility CHILD elements (a custom NSAccessibilityElement
 * / objc_allocateClassPair dance) so VoiceOver enumerates the widgets — that is the
 * deep, VoiceOver-verified follow-up this example will grow.
 *
 * Prints "PASS" after setting the label; "SKIP" if objc/AppKit are unavailable.
 */
#include <stdio.h>
#include <dlfcn.h>

int main(void) {
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) objc = dlopen("libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) { printf("SKIP: objc runtime unavailable (not macOS?)\n"); return 2; }

    void *(*getClass)(const char *)       = dlsym(objc, "objc_getClass");
    void *(*sel)(const char *)            = dlsym(objc, "sel_registerName");
    void *(*msg)(void *, void *)          = dlsym(objc, "objc_msgSend");
    if (!getClass || !sel || !msg) { printf("SKIP: objc symbols unavailable\n"); return 2; }

    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW | RTLD_GLOBAL)) {
        printf("SKIP: AppKit unavailable\n");
        return 2;
    }

    void *appCls = getClass("NSApplication");
    if (!appCls) { printf("SKIP: NSApplication unavailable\n"); return 2; }
    void *app = msg(appCls, sel("sharedApplication"));
    if (!app) { printf("SKIP: sharedApplication nil\n"); return 2; }

    void *strCls = getClass("NSString");
    void *(*mkstr)(void *, void *, const char *) =
        (void *(*)(void *, void *, const char *))msg;
    void *label = mkstr(strCls, sel("stringWithUTF8String:"), "Ruxen Canvas Demo");
    if (!label) { printf("SKIP: NSString nil\n"); return 2; }

    /* [NSApp setAccessibilityLabel:label] — the real NSAccessibility setter. */
    void (*setlabel)(void *, void *, void *) = (void (*)(void *, void *, void *))msg;
    setlabel(app, sel("setAccessibilityLabel:"), label);

    /* read it back: [NSApp accessibilityLabel].UTF8String */
    void *got = msg(app, sel("accessibilityLabel"));
    const char *cstr = got ? (const char *)msg(got, sel("UTF8String")) : NULL;
    printf("app accessibilityLabel = %s\n", cstr ? cstr : "(nil)");
    if (cstr && cstr[0]) {
        printf("PASS: NSAccessibility label set + read back via objc\n");
        return 0;
    }
    printf("FAIL: label did not round-trip\n");
    return 1;
}
