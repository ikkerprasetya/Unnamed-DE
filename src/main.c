#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int main(void) {
    Display *dpy;
    Window root;
    XWindowAttributes attr;
    XButtonEvent start;
    XEvent ev;

    // 1. Initialize connection to X Server
    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "Cannot open X display\n");
        exit(1);
    }

    root = DefaultRootWindow(dpy);

    // 2. Bind hotkeys to the root window 
    // We will use Alt (Mod1Mask) + F1 to raise windows, and Alt + Mouse buttons to move/resize
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_F1), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabButton(dpy, 1, Mod1Mask, root, True, ButtonPressMask|ButtonReleaseMask|PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, 3, Mod1Mask, root, True, ButtonPressMask|ButtonReleaseMask|PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);

    start.subwindow = None;

    // 3. The Main Event Loop
    while (1) {
        XNextEvent(dpy, &ev);

        // Intercept ButtonPress (Alt + Left Click or Alt + Right Click)
        if (ev.type == ButtonPress && ev.xbutton.subwindow != None) {
            XGetWindowAttributes(dpy, ev.xbutton.subwindow, &attr);
            start = ev.xbutton;
        }
        // Intercept Mouse Motion while a button is held down
        else if (ev.type == MotionNotify && start.subwindow != None) {
            int xdiff = ev.xbutton.x_root - start.x_root;
            int ydiff = ev.xbutton.y_root - start.y_root;

            XMoveResizeWindow(dpy, start.subwindow,
                attr.x + (start.button == 1 ? xdiff : 0),
                attr.y + (start.button == 1 ? ydiff : 0),
                MAX(1, attr.width + (start.button == 3 ? xdiff : 0)),
                MAX(1, attr.height + (start.button == 3 ? ydiff : 0)));
        }
        // Intercept KeyPress (Alt + F1)
        else if (ev.type == KeyPress && ev.xkey.subwindow != None) {
            XRaiseWindow(dpy, ev.xkey.subwindow);
        }
        // Clean up tracking when button is released
        else if (ev.type == ButtonRelease) {
            start.subwindow = None;
        }
    }

    XCloseDisplay(dpy);
    return 0;
}


