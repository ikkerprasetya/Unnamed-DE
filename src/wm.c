#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#define PANEL_HEIGHT 32
#define TITLE_HEIGHT 20
#define BUTTON_WIDTH 24
#define MAX_CLIENTS 64
#define WORKSPACES 4
#define MIN_WIDTH 120
#define MIN_HEIGHT 60
#define MARGIN 12

enum { KIND_NORMAL, KIND_PANEL, KIND_DESKTOP, KIND_NOTIFY };
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };

typedef struct {
    int used;
    Window client;
    Window frame;
    int workspace;
    int x;
    int y;
    int width;
    int height;
} Client;

static Display *display;
static Window root;
static GC gc;
static Client clients[MAX_CLIENTS];
static int active = MAX_CLIENTS;
static int current = 0;
static int screen_number;
static int screen_width;
static int screen_height;
static int body_height;
static volatile sig_atomic_t running = 1;
static Window panel;
static Window desktop;
static Window notice;
static int drag_mode = DRAG_NONE;
static int drag_index;
static int drag_pointer_x;
static int drag_pointer_y;
static int drag_x;
static int drag_y;
static int drag_width;
static int drag_height;
static Atom atom_client_list;
static Atom atom_active_window;
static Atom atom_current_desktop;
static Atom atom_number_of_desktops;
static Atom atom_close_window;
static Atom atom_name;
static Atom atom_utf8;
static Atom atom_protocols;
static Atom atom_delete;

static int subtract(int a, int b) {
    return a + (~b + 1);
}

static int ignore_error(Display *d, XErrorEvent *e) {
    return 0;
}

static void on_signal(int signal_number) {
    running = 0;
}

static void spawn(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl(path, path, (char *)NULL);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static void get_title(Window w, char *buffer, int size) {
    char *name = NULL;
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;

    buffer[0] = '\0';
    if (XGetWindowProperty(display, w, atom_name, 0, 128, False, atom_utf8,
                           &type, &format, &count, &after, &data) == Success && data != NULL) {
        snprintf(buffer, size, "%s", (char *)data);
        XFree(data);
    }
    if (buffer[0] != '\0') {
        return;
    }
    if (XFetchName(display, w, &name) && name != NULL) {
        snprintf(buffer, size, "%s", name);
        XFree(name);
    }
}

static int kind_of(Window w) {
    char title[256];
    get_title(w, title, sizeof(title));
    if (strcmp(title, "panel_de") == 0) {
        return KIND_PANEL;
    }
    if (strcmp(title, "desktop_de") == 0) {
        return KIND_DESKTOP;
    }
    if (strcmp(title, "notify_de") == 0) {
        return KIND_NOTIFY;
    }
    return KIND_NORMAL;
}

static int find_by_client(Window w) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].client == w) {
            return i;
        }
    }
    return MAX_CLIENTS;
}

static int find_by_frame(Window w) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].frame == w) {
            return i;
        }
    }
    return MAX_CLIENTS;
}

static int first_in_workspace(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].workspace == current) {
            return i;
        }
    }
    return MAX_CLIENTS;
}

static void publish(void) {
    unsigned long list[MAX_CLIENTS];
    unsigned long count = 0;
    unsigned long number = WORKSPACES;
    unsigned long desktop_value = (unsigned long)current;
    unsigned long active_value = 0;
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used) {
            list[count] = clients[i].client;
            count++;
        }
    }
    if (active != MAX_CLIENTS) {
        active_value = clients[active].client;
    }
    XChangeProperty(display, root, atom_client_list, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)list, (int)count);
    XChangeProperty(display, root, atom_number_of_desktops, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&number, 1);
    XChangeProperty(display, root, atom_current_desktop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&desktop_value, 1);
    XChangeProperty(display, root, atom_active_window, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&active_value, 1);
    XFlush(display);
}

static void draw_frame(int i) {
    char title[256];
    int button_x = subtract(clients[i].width, BUTTON_WIDTH) + 8;

    get_title(clients[i].client, title, sizeof(title));
    XClearWindow(display, clients[i].frame);
    XDrawString(display, clients[i].frame, gc, 6, 14, title, (int)strlen(title));
    XDrawString(display, clients[i].frame, gc, button_x, 14, "x", 1);
    XDrawLine(display, clients[i].frame, gc, 0, TITLE_HEIGHT, clients[i].width, TITLE_HEIGHT);
}

static void send_configure(int i) {
    XConfigureEvent configure;

    memset(&configure, 0, sizeof(configure));
    configure.type = ConfigureNotify;
    configure.display = display;
    configure.event = clients[i].client;
    configure.window = clients[i].client;
    configure.x = clients[i].x + 1;
    configure.y = clients[i].y + TITLE_HEIGHT + 1;
    configure.width = clients[i].width;
    configure.height = subtract(clients[i].height, TITLE_HEIGHT);
    configure.border_width = 0;
    configure.above = None;
    configure.override_redirect = False;
    XSendEvent(display, clients[i].client, False, StructureNotifyMask, (XEvent *)&configure);
}

static void focus_client(int i, int raise) {
    if (i == MAX_CLIENTS) {
        active = MAX_CLIENTS;
        XSetInputFocus(display, root, RevertToPointerRoot, CurrentTime);
        publish();
        return;
    }
    if (raise) {
        XRaiseWindow(display, clients[i].frame);
        if (notice != 0) {
            XRaiseWindow(display, notice);
        }
    }
    XSetInputFocus(display, clients[i].client, RevertToPointerRoot, CurrentTime);
    active = i;
    publish();
}

static void switch_workspace(int n) {
    int i;

    if (n < 0 || n >= WORKSPACES) {
        return;
    }
    current = n;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            continue;
        }
        if (clients[i].workspace == current) {
            XMapWindow(display, clients[i].frame);
        } else {
            XUnmapWindow(display, clients[i].frame);
        }
    }
    focus_client(first_in_workspace(), 1);
}

static void close_client(int i) {
    Atom *protocols = NULL;
    int count = 0;
    int n;
    int supported = 0;
    XEvent message;

    if (i == MAX_CLIENTS) {
        return;
    }
    if (XGetWMProtocols(display, clients[i].client, &protocols, &count)) {
        for (n = 0; n < count; n++) {
            if (protocols[n] == atom_delete) {
                supported = 1;
            }
        }
        XFree(protocols);
    }
    if (!supported) {
        XKillClient(display, clients[i].client);
        return;
    }
    memset(&message, 0, sizeof(message));
    message.xclient.type = ClientMessage;
    message.xclient.window = clients[i].client;
    message.xclient.message_type = atom_protocols;
    message.xclient.format = 32;
    message.xclient.data.l[0] = (long)atom_delete;
    message.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, clients[i].client, False, NoEventMask, &message);
    XFlush(display);
}

static void frame_client(Window w) {
    XWindowAttributes attributes;
    static int cascade = 0;
    int i;
    int width;
    int height;
    int offset;
    int max_height;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            break;
        }
    }
    if (i == MAX_CLIENTS) {
        XMapWindow(display, w);
        return;
    }
    XGetWindowAttributes(display, w, &attributes);
    width = attributes.width;
    height = attributes.height;
    max_height = subtract(screen_height, PANEL_HEIGHT + TITLE_HEIGHT);
    if (width < MIN_WIDTH) {
        width = MIN_WIDTH;
    }
    if (width > screen_width) {
        width = screen_width;
    }
    if (height < MIN_HEIGHT) {
        height = MIN_HEIGHT;
    }
    if (height > max_height) {
        height = max_height;
    }
    offset = (cascade % 8) * 24;
    cascade++;

    clients[i].used = 1;
    clients[i].client = w;
    clients[i].workspace = current;
    clients[i].x = 40 + offset;
    clients[i].y = PANEL_HEIGHT + 20 + offset;
    clients[i].width = width;
    clients[i].height = height + TITLE_HEIGHT;
    if (clients[i].x + clients[i].width > screen_width) {
        clients[i].x = subtract(screen_width, clients[i].width);
    }
    if (clients[i].y + clients[i].height > screen_height) {
        clients[i].y = subtract(screen_height, clients[i].height);
    }
    if (clients[i].y < PANEL_HEIGHT) {
        clients[i].y = PANEL_HEIGHT;
    }
    clients[i].frame = XCreateSimpleWindow(display, root, clients[i].x, clients[i].y,
                                           clients[i].width, clients[i].height, 1,
                                           BlackPixel(display, screen_number),
                                           WhitePixel(display, screen_number));
    XSelectInput(display, clients[i].frame,
                 SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask |
                 ExposureMask | EnterWindowMask);
    XSelectInput(display, w, PropertyChangeMask);
    XAddToSaveSet(display, w);
    XResizeWindow(display, w, width, height);
    XReparentWindow(display, w, clients[i].frame, 0, TITLE_HEIGHT);
    XMapWindow(display, w);
    XMapWindow(display, clients[i].frame);
    send_configure(i);
    focus_client(i, 1);
}

static void remove_client(int i, int alive) {
    if (alive) {
        XReparentWindow(display, clients[i].client, root, clients[i].x, clients[i].y);
        XRemoveFromSaveSet(display, clients[i].client);
    }
    XDestroyWindow(display, clients[i].frame);
    clients[i].used = 0;
    if (drag_mode != DRAG_NONE && drag_index == i) {
        drag_mode = DRAG_NONE;
        XUngrabPointer(display, CurrentTime);
    }
    if (active == i) {
        focus_client(first_in_workspace(), 1);
    } else {
        publish();
    }
}

static void on_map_request(Window w) {
    XWindowAttributes attributes;
    int kind;

    if (find_by_client(w) != MAX_CLIENTS) {
        XMapWindow(display, w);
        return;
    }
    kind = kind_of(w);
    if (kind == KIND_PANEL) {
        panel = w;
        XMoveResizeWindow(display, w, 0, 0, screen_width, PANEL_HEIGHT);
        XMapWindow(display, w);
        XRaiseWindow(display, w);
        return;
    }
    if (kind == KIND_DESKTOP) {
        desktop = w;
        XMoveResizeWindow(display, w, 0, PANEL_HEIGHT, screen_width, body_height);
        XMapWindow(display, w);
        XLowerWindow(display, w);
        return;
    }
    if (kind == KIND_NOTIFY) {
        notice = w;
        XGetWindowAttributes(display, w, &attributes);
        XMoveWindow(display, w, subtract(screen_width, attributes.width + MARGIN),
                    PANEL_HEIGHT + MARGIN);
        XMapWindow(display, w);
        XRaiseWindow(display, w);
        return;
    }
    frame_client(w);
}

static void on_configure_request(XConfigureRequestEvent request) {
    XWindowChanges changes;
    int i = find_by_client(request.window);

    if (i != MAX_CLIENTS) {
        if (request.value_mask & CWWidth) {
            clients[i].width = request.width;
        }
        if (request.value_mask & CWHeight) {
            clients[i].height = request.height + TITLE_HEIGHT;
        }
        XResizeWindow(display, clients[i].frame, clients[i].width, clients[i].height);
        XResizeWindow(display, clients[i].client, clients[i].width,
                      subtract(clients[i].height, TITLE_HEIGHT));
        send_configure(i);
        draw_frame(i);
        return;
    }
    if (request.window == panel || request.window == desktop) {
        return;
    }
    if (request.window == notice) {
        XMoveResizeWindow(display, request.window,
                          subtract(screen_width, request.width + MARGIN),
                          PANEL_HEIGHT + MARGIN, request.width, request.height);
        return;
    }
    changes.x = request.x;
    changes.y = request.y;
    changes.width = request.width;
    changes.height = request.height;
    changes.border_width = request.border_width;
    changes.sibling = request.above;
    changes.stack_mode = request.detail;
    XConfigureWindow(display, request.window, (unsigned int)request.value_mask, &changes);
}

static void on_unmap(Window w) {
    int i = find_by_client(w);
    if (i != MAX_CLIENTS) {
        remove_client(i, 1);
    }
}

static void on_destroy(Window w) {
    int i = find_by_client(w);
    if (w == panel) {
        panel = 0;
    }
    if (w == desktop) {
        desktop = 0;
    }
    if (w == notice) {
        notice = 0;
    }
    if (i != MAX_CLIENTS) {
        remove_client(i, 0);
    }
}

static void cycle(void) {
    int start = active;
    int step;
    int i;

    for (step = 1; step <= MAX_CLIENTS; step++) {
        i = (start + step) % MAX_CLIENTS;
        if (clients[i].used && clients[i].workspace == current && i != active) {
            focus_client(i, 1);
            return;
        }
    }
}

static void on_key(XKeyEvent key) {
    KeySym symbol;
    int shift = (key.state & ShiftMask) != 0;
    int number;

    if ((key.state & Mod1Mask) == 0) {
        return;
    }
    symbol = XLookupKeysym(&key, 0);
    if (symbol == XK_Return) {
        spawn("./terminal");
    } else if (symbol == XK_f) {
        spawn("./files");
    } else if (symbol == XK_s) {
        spawn("./settings");
    } else if (symbol == XK_q || symbol == XK_F4) {
        close_client(active);
    } else if (symbol == XK_Tab) {
        cycle();
    } else if (symbol == XK_e) {
        running = 0;
    } else if (symbol >= XK_1 && symbol < XK_1 + WORKSPACES) {
        number = subtract((int)symbol, (int)XK_1);
        if (!shift) {
            switch_workspace(number);
        } else if (active != MAX_CLIENTS) {
            clients[active].workspace = number;
            if (number != current) {
                XUnmapWindow(display, clients[active].frame);
                focus_client(first_in_workspace(), 1);
            }
        }
    }
}

static void start_drag(int i, int mode, XButtonEvent press) {
    drag_mode = mode;
    drag_index = i;
    drag_pointer_x = press.x_root;
    drag_pointer_y = press.y_root;
    drag_x = clients[i].x;
    drag_y = clients[i].y;
    drag_width = clients[i].width;
    drag_height = clients[i].height;
    XGrabPointer(display, root, False, ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

static void on_button_press(XButtonEvent press) {
    int alt = (press.state & Mod1Mask) != 0;
    Window target = alt ? press.subwindow : press.window;
    int i = find_by_frame(target);

    if (i == MAX_CLIENTS) {
        return;
    }
    focus_client(i, 1);
    if (alt && press.button == Button1) {
        start_drag(i, DRAG_MOVE, press);
    } else if (alt && press.button == Button3) {
        start_drag(i, DRAG_RESIZE, press);
    } else if (!alt && press.button == Button1 && press.y < TITLE_HEIGHT) {
        if (press.x >= subtract(clients[i].width, BUTTON_WIDTH)) {
            close_client(i);
        } else {
            start_drag(i, DRAG_MOVE, press);
        }
    }
}

static void on_motion(XMotionEvent motion) {
    int dx;
    int dy;
    int x;
    int y;
    int width;
    int height;

    if (drag_mode == DRAG_NONE || !clients[drag_index].used) {
        return;
    }
    dx = subtract(motion.x_root, drag_pointer_x);
    dy = subtract(motion.y_root, drag_pointer_y);
    if (drag_mode == DRAG_MOVE) {
        x = drag_x + dx;
        y = drag_y + dy;
        if (y < PANEL_HEIGHT) {
            y = PANEL_HEIGHT;
        }
        clients[drag_index].x = x;
        clients[drag_index].y = y;
        XMoveWindow(display, clients[drag_index].frame, x, y);
        send_configure(drag_index);
        return;
    }
    width = drag_width + dx;
    height = drag_height + dy;
    if (width < MIN_WIDTH) {
        width = MIN_WIDTH;
    }
    if (height < MIN_HEIGHT + TITLE_HEIGHT) {
        height = MIN_HEIGHT + TITLE_HEIGHT;
    }
    clients[drag_index].width = width;
    clients[drag_index].height = height;
    XResizeWindow(display, clients[drag_index].frame, width, height);
    XResizeWindow(display, clients[drag_index].client, width, subtract(height, TITLE_HEIGHT));
    send_configure(drag_index);
    draw_frame(drag_index);
}

static void on_client_message(XClientMessageEvent message) {
    int i;

    if (message.message_type == atom_active_window) {
        i = find_by_client(message.window);
        if (i == MAX_CLIENTS) {
            return;
        }
        if (clients[i].workspace != current) {
            switch_workspace(clients[i].workspace);
        }
        focus_client(i, 1);
    } else if (message.message_type == atom_current_desktop) {
        switch_workspace((int)message.data.l[0]);
    } else if (message.message_type == atom_close_window) {
        close_client(find_by_client(message.window));
    }
}

static void bind_keys(void) {
    KeySym symbols[11] = {XK_Return, XK_f, XK_s, XK_q, XK_Tab, XK_e,
                          XK_1, XK_2, XK_3, XK_4, XK_F4};
    unsigned int variants[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    unsigned int shifts[2] = {0, ShiftMask};
    unsigned int buttons[2] = {Button1, Button3};
    int i;
    int j;
    int k;

    for (i = 0; i < 11; i++) {
        KeyCode code = XKeysymToKeycode(display, symbols[i]);
        for (j = 0; j < 4; j++) {
            for (k = 0; k < 2; k++) {
                XGrabKey(display, code, Mod1Mask | variants[j] | shifts[k], root, True,
                         GrabModeAsync, GrabModeAsync);
            }
        }
    }
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 4; j++) {
            XGrabButton(display, buttons[i], Mod1Mask | variants[j], root, True,
                        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                        GrabModeAsync, GrabModeAsync, None, None);
        }
    }
}

int main(void) {
    XEvent event;
    fd_set fds;
    struct timeval interval;
    char pid[32];
    int connection;
    int i;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        return 1;
    }
    connection = ConnectionNumber(display);
    screen_number = DefaultScreen(display);
    root = RootWindow(display, screen_number);
    screen_width = DisplayWidth(display, screen_number);
    screen_height = DisplayHeight(display, screen_number);
    body_height = subtract(screen_height, PANEL_HEIGHT);

    XSetErrorHandler(ignore_error);
    atom_client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    atom_active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    atom_current_desktop = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
    atom_number_of_desktops = XInternAtom(display, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_close_window = XInternAtom(display, "_NET_CLOSE_WINDOW", False);
    atom_name = XInternAtom(display, "_NET_WM_NAME", False);
    atom_utf8 = XInternAtom(display, "UTF8_STRING", False);
    atom_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    atom_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);

    gc = XCreateGC(display, root, 0, NULL);
    XSetForeground(display, gc, BlackPixel(display, screen_number));
    XDefineCursor(display, root, XCreateFontCursor(display, XC_left_ptr));
    XSelectInput(display, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(display, False);
    bind_keys();
    publish();

    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    setenv("DE_SESSION_PID", pid, 1);
    spawn("./desktop");
    spawn("./panel");
    spawn("./notify");

    while (running) {
        if (XPending(display) == 0) {
            FD_ZERO(&fds);
            FD_SET(connection, &fds);
            interval.tv_sec = 1;
            interval.tv_usec = 0;
            select(connection + 1, &fds, NULL, NULL, &interval);
            continue;
        }
        XNextEvent(display, &event);
        switch (event.type) {
        case MapRequest:
            on_map_request(event.xmaprequest.window);
            break;
        case ConfigureRequest:
            on_configure_request(event.xconfigurerequest);
            break;
        case UnmapNotify:
            on_unmap(event.xunmap.window);
            break;
        case DestroyNotify:
            on_destroy(event.xdestroywindow.window);
            break;
        case KeyPress:
            on_key(event.xkey);
            break;
        case ButtonPress:
            on_button_press(event.xbutton);
            break;
        case ButtonRelease:
            if (drag_mode != DRAG_NONE) {
                drag_mode = DRAG_NONE;
                XUngrabPointer(display, CurrentTime);
            }
            break;
        case MotionNotify:
            on_motion(event.xmotion);
            break;
        case Expose:
            i = find_by_frame(event.xexpose.window);
            if (i != MAX_CLIENTS && event.xexpose.count == 0) {
                draw_frame(i);
            }
            break;
        case EnterNotify:
            i = find_by_frame(event.xcrossing.window);
            if (i != MAX_CLIENTS && event.xcrossing.mode == NotifyNormal &&
                drag_mode == DRAG_NONE && i != active) {
                focus_client(i, 0);
            }
            break;
        case PropertyNotify:
            i = find_by_client(event.xproperty.window);
            if (i != MAX_CLIENTS) {
                draw_frame(i);
            }
            break;
        case ClientMessage:
            on_client_message(event.xclient);
            break;
        }
    }

    XCloseDisplay(display);
    return 0;
}