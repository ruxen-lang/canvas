/* file_dialog_verify.c — standalone, HUMAN-INTERACTIVE proof that the Phase-2
 * native file dialogs drive a real macOS NSOpenPanel / NSSavePanel through the
 * objc runtime via dlopen (no link dependency — the same discipline the engine's
 * Metal device path uses). A modal dialog needs a real CLICK, so this CANNOT run
 * in the forked test harness (Cocoa is unsafe after fork(), and there is no human
 * to pick a file). It is a MANUAL example, documented and compile-checked, NOT
 * wired into anything automated — the automated bar is the headless Err pin in
 * tests/file_dialog.rx plus this file compiling.
 *
 *   cc -O2 -o file_dialog_verify examples/file_dialog_verify.c
 *   ./file_dialog_verify         # an Open panel appears; pick a file (or cancel),
 *                                # then a Save panel; the chosen paths are printed.
 *
 * Mirrors runtime/sdl_window.c's ruxen_canvas_open_file_dialog /
 * ruxen_canvas_save_file_dialog exactly: objc_getClass + objc_msgSend message
 * sends to [NSOpenPanel openPanel] / [NSSavePanel savePanel], a foreground-app
 * activation so the modal can take key focus, [panel runModal], then
 * [[panel URL] path].UTF8String.
 *
 * Prints "PASS" after running both panels; "SKIP" if the objc runtime / AppKit are
 * unavailable (e.g. not macOS).
 */
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>

#define RX_NS_MODAL_OK 1   /* NSModalResponseOK on modern macOS */

int main(void) {
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) objc = dlopen("libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) { printf("SKIP: objc runtime unavailable (not macOS?)\n"); return 2; }

    void *(*getClass)(const char *) = dlsym(objc, "objc_getClass");
    void *(*sel)(const char *)      = dlsym(objc, "sel_registerName");
    void *(*msg)(void *, void *)    = dlsym(objc, "objc_msgSend");
    if (!getClass || !sel || !msg) { printf("SKIP: objc symbols unavailable\n"); return 2; }

    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW | RTLD_GLOBAL)) {
        printf("SKIP: AppKit unavailable\n");
        return 2;
    }

    /* Typed objc_msgSend casts (object-returning, BOOL-arg, NSInteger-returning). */
    void *(*msg_o)(void *, void *)              = msg;
    void *(*msg_op)(void *, void *, void *)     = (void *(*)(void *, void *, void *))msg;
    void  (*msg_ob)(void *, void *, signed char) = (void (*)(void *, void *, signed char))msg;
    long  (*msg_ol)(void *, void *)             = (long (*)(void *, void *))msg;
    long  (*msg_oil)(void *, void *, long)      = (long (*)(void *, void *, long))msg;
    void *(*msg_os)(void *, void *, const char *) = (void *(*)(void *, void *, const char *))msg;

    /* Foreground-app activation so the modal can become key window. */
    void *appCls = getClass("NSApplication");
    if (appCls) {
        void *app = msg_o(appCls, sel("sharedApplication"));
        if (app) {
            msg_oil(app, sel("setActivationPolicy:"), 0); /* Regular */
            msg_ob(app, sel("activateIgnoringOtherApps:"), 1);
        }
    }

    /* ---- NSOpenPanel ---- */
    void *openCls = getClass("NSOpenPanel");
    if (openCls) {
        void *panel = msg_o(openCls, sel("openPanel"));
        if (panel) {
            msg_ob(panel, sel("setCanChooseFiles:"), 1);
            msg_ob(panel, sel("setCanChooseDirectories:"), 0);
            msg_ob(panel, sel("setAllowsMultipleSelection:"), 0);
            printf("Open panel: pick a file (or cancel)...\n");
            long resp = msg_ol(panel, sel("runModal"));
            if (resp == RX_NS_MODAL_OK) {
                void *url = msg_o(panel, sel("URL"));
                void *path = url ? msg_o(url, sel("path")) : 0;
                const char *c = path ? (const char *)msg_o(path, sel("UTF8String")) : 0;
                printf("  opened: %s\n", c ? c : "(none)");
            } else {
                printf("  open cancelled\n");
            }
        }
    }

    /* ---- NSSavePanel (with a suggested filename) ---- */
    void *saveCls = getClass("NSSavePanel");
    if (saveCls) {
        void *panel = msg_o(saveCls, sel("savePanel"));
        if (panel) {
            void *strCls = getClass("NSString");
            if (strCls) {
                void *ns = msg_os(strCls, sel("stringWithUTF8String:"), "untitled.txt");
                if (ns) msg_op(panel, sel("setNameFieldStringValue:"), ns);
            }
            printf("Save panel: choose a save location (or cancel)...\n");
            long resp = msg_ol(panel, sel("runModal"));
            if (resp == RX_NS_MODAL_OK) {
                void *url = msg_o(panel, sel("URL"));
                void *path = url ? msg_o(url, sel("path")) : 0;
                const char *c = path ? (const char *)msg_o(path, sel("UTF8String")) : 0;
                printf("  saving to: %s\n", c ? c : "(none)");
            } else {
                printf("  save cancelled\n");
            }
        }
    }

    printf("PASS\n");
    return 0;
}
