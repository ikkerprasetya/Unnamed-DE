#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PANEL_HEIGHT 32
#define TITLE_HEIGHT 20
#define BUTTON_WIDTH 24
#define MAX_CLIENTS 64
#define WORKSPACES 4
#define MIN_WIDTH 120
#define MIN_HEIGHT 60
#define MARGIN 12
#define KEY_COUNT 12
#define ANIMATION_MS 170
#define FRAME_MS 16
#define OPEN_SLIDE 24
#define DOUBLE_CLICK_MS 300

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
    int ax;
    int ay;
    int awidth;
    int aheight;
    int animating;
    long start;
    int from_x;
    int from_y;
    int from_width;
    int from_height;
    int maximized;
    int saved_x;
    int saved_y;
    int saved_width;
    int saved_height;
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
static int other_wm = 0;
static volatile sig_atomic_t running = 1;
static Window panel;
static Window desktop;
static Window notice;
static Window check;
static int desktop_focused = 0;
static int drag_mode = DRAG_NONE;
static int drag_index;
static int drag_pointer_x;
static int drag_pointer_y;
static int drag_x;
static int drag_y;
static int drag_width;
static int drag_height;
static int last_click_index = MAX_CLIENTS;
static Time last_click_time = 0;
static long last_tick = 0;
static Atom atom_supported;
static Atom atom_client_list;
static Atom atom_active_window;
static Atom atom_current_desktop;
static Atom atom_number_of_desktops;
static Atom atom_close_window;
static Atom atom_workarea;
static Atom atom_extents;
static Atom atom_check;
static Atom atom_name;
static Atom atom_utf8;
static Atom atom_protocols;
static Atom atom_delete;

static int subtract(int a, int b) {
    return a + (~b + 1);
}

static long subtract_long(long a, long b) {
    return a + (~b + 1);
}

static long now_ms(void) {
    struct timespec spec;

    clock_gettime(CLOCK_MONOTONIC, &spec);
    return (long)spec.tv_sec * 1000 + spec.tv_nsec / 1000000;
}

static int ignore_error(Display *d, XErrorEvent *e) {
    (void)d;
    (void)e;
    return 0;
}

static int startup_error(Display *d, XErrorEvent *e) {
    (void)d;
    if ((*e).error_code == BadAccess) {
        other_wm = 1;
    }
    return 0;
}

static void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void spawn(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl(path, path, (char *)NULL);
            _exit(1);
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
        if (type == atom_utf8 && format == 8) {
            snprintf(buffer, size, "%s", (char *)data);
        }
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

static void publish_static(void) {
    unsigned long area[4 * WORKSPACES];
    unsigned long check_value;
    int i;

    for (i = 0; i < WORKSPACES; i++) {
        area[i * 4] = 0;
        area[i * 4 + 1] = PANEL_HEIGHT;
        area[i * 4 + 2] = (unsigned long)screen_width;
        area[i * 4 + 3] = (unsigned long)body_height;
    }
    XChangeProperty(display, root, atom_workarea, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)area, 4 * WORKSPACES);
    check = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    check_value = check;
    XChangeProperty(display, root, atom_check, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check_value, 1);
    XChangeProperty(display, check, atom_check, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check_value, 1);
    XChangeProperty(display, check, atom_name, atom_utf8, 8, PropModeReplace,
                    (unsigned char *)"bare_de", 7);
}

static void set_extents(Window w) {
    unsigned long extents[4] = {1, 1, TITLE_HEIGHT + 1, 1};

    XChangeProperty(display, w, atom_extents, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)extents, 4);
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
    int button_x = subtract(clients[i].awidth, BUTTON_WIDTH) + 8;

    get_title(clients[i].client, title, sizeof(title));
    XClearWindow(display, clients[i].frame);
    XDrawString(display, clients[i].frame, gc, 6, 14, title, (int)strlen(title));
    XDrawString(display, clients[i].frame, gc, button_x, 14, "x", 1);
    XDrawLine(display, clients[i].frame, gc, 0, TITLE_HEIGHT, clients[i].awidth, TITLE_HEIGHT);
}

static void send_configure(int i) {
    XConfigureEvent configure;

    memset(&configure, 0, sizeof(configure));
    configure.type = ConfigureNotify;
    configure.display = display;
    configure.event = clients[i].client;
    configure.window = clients[i].client;
    configure.x = clients[i].ax + 1;
    configure.y = clients[i].ay + TITLE_HEIGHT + 1;
    configure.width = clients[i].awidth;
    configure.height = subtract(clients[i].aheight, TITLE_HEIGHT);
    configure.border_width = 0;
    configure.above = None;
    configure.override_redirect = False;
    XSendEvent(display, clients[i].client, False, StructureNotifyMask, (XEvent *)&configure);
}

static void place(int i, int x, int y, int width, int height) {
    int resized = width != clients[i].awidth || height != clients[i].aheight;

    clients[i].ax = x;
    clients[i].ay = y;
    clients[i].awidth = width;
    clients[i].aheight = height;
    if (resized) {
        XMoveResizeWindow(display, clients[i].frame, x, y, width, height);
        XResizeWindow(display, clients[i].client, width, subtract(height, TITLE_HEIGHT));
    } else {
        XMoveWindow(display, clients[i].frame, x, y);
    }
    send_configure(i);
    if (resized) {
        draw_frame(i);
    }
}

static void set_geometry(int i, int x, int y, int width, int height) {
    clients[i].animating = 0;
    clients[i].x = x;
    clients[i].y = y;
    clients[i].width = width;
    clients[i].height = height;
    place(i, x, y, width, height);
}

static void animate_to(int i, int x, int y, int width, int height) {
    clients[i].from_x = clients[i].ax;
    clients[i].from_y = clients[i].ay;
    clients[i].from_width = clients[i].awidth;
    clients[i].from_height = clients[i].aheight;
    clients[i].x = x;
    clients[i].y = y;
    clients[i].width = width;
    clients[i].height = height;
    clients[i].start = now_ms();
    clients[i].animating = 1;
}

static void settle(int i) {
    if (clients[i].animating) {
        clients[i].animating = 0;
        place(i, clients[i].x, clients[i].y, clients[i].width, clients[i].height);
    }
}

static int lerp(int from, int to, int eased) {
    return from + subtract(to, from) * eased / 1000;
}

static int any_animating(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].animating) {
            return 1;
        }
    }
    return 0;
}

static void tick_animations(void) {
    long now;
    int i;
    int elapsed;
    int progress;
    int inverse;
    int eased;

    if (!any_animating()) {
        return;
    }
    now = now_ms();
    if (now < last_tick + FRAME_MS) {
        return;
    }
    last_tick = now;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used || !clients[i].animating) {
            continue;
        }
        elapsed = (int)subtract_long(now, clients[i].start);
        if (elapsed >= ANIMATION_MS) {
            settle(i);
            continue;
        }
        progress = elapsed * 1000 / ANIMATION_MS;
        inverse = subtract(1000, progress);
        eased = subtract(1000, inverse * inverse * inverse / 1000000);
        place(i, lerp(clients[i].from_x, clients[i].x, eased),
              lerp(clients[i].from_y, clients[i].y, eased),
              lerp(clients[i].from_width, clients[i].width, eased),
              lerp(clients[i].from_height, clients[i].height, eased));
    }
    XFlush(display);
}

static void toggle_maximize(int i) {
    if (i == MAX_CLIENTS) {
        return;
    }
    if (clients[i].maximized) {
        clients[i].maximized = 0;
        animate_to(i, clients[i].saved_x, clients[i].saved_y,
                   clients[i].saved_width, clients[i].saved_height);
        return;
    }
    clients[i].saved_x = clients[i].x;
    clients[i].saved_y = clients[i].y;
    clients[i].saved_width = clients[i].width;
    clients[i].saved_height = clients[i].height;
    clients[i].maximized = 1;
    animate_to(i, 0, PANEL_HEIGHT, subtract(screen_width, 2), subtract(body_height, 2));
}

static void focus_client(int i, int raise) {
    if (i == MAX_CLIENTS) {
        active = MAX_CLIENTS;
        desktop_focused = 0;
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
    desktop_focused = 0;
    XSetInputFocus(display, clients[i].client, RevertToPointerRoot, CurrentTime);
    active = i;
    publish();
}

static void focus_desktop(void) {
    if (desktop == 0 || desktop_focused) {
        return;
    }
    desktop_focused = 1;
    active = MAX_CLIENTS;
    XSetInputFocus(display, desktop, RevertToPointerRoot, CurrentTime);
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
    if (!XGetWindowAttributes(display, w, &attributes)) {
        return;
    }
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

    memset(&clients[i], 0, sizeof(clients[i]));
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
    clients[i].ax = clients[i].x;
    clients[i].ay = clients[i].y + OPEN_SLIDE;
    clients[i].awidth = clients[i].width;
    clients[i].aheight = clients[i].height;
    clients[i].frame = XCreateSimpleWindow(display, root, clients[i].ax, clients[i].ay,
                                           clients[i].awidth, clients[i].aheight, 1,
                                           BlackPixel(display, screen_number),
                                           WhitePixel(display, screen_number));
    XSelectInput(display, clients[i].frame,
                 SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask |
                 ExposureMask | EnterWindowMask);
    XSelectInput(display, w, PropertyChangeMask);
    XAddToSaveSet(display, w);
    set_extents(w);
    XResizeWindow(display, w, width, height);
    XReparentWindow(display, w, clients[i].frame, 0, TITLE_HEIGHT);
    XMapWindow(display, w);
    XMapWindow(display, clients[i].frame);
    send_configure(i);
    focus_client(i, 1);
    animate_to(i, clients[i].x, clients[i].y, clients[i].width, clients[i].height);
}

static void remove_client(int i, int alive) {
    if (alive) {
        XReparentWindow(display, clients[i].client, root, clients[i].ax, clients[i].ay);
        XRemoveFromSaveSet(display, clients[i].client);
    }
    XDestroyWindow(display, clients[i].frame);
    clients[i].used = 0;
    clients[i].animating = 0;
    if (last_click_index == i) {
        last_click_index = MAX_CLIENTS;
    }
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
        XSelectInput(display, w, EnterWindowMask);
        XMoveResizeWindow(display, w, 0, PANEL_HEIGHT, screen_width, body_height);
        XMapWindow(display, w);
        XLowerWindow(display, w);
        return;
    }
    if (kind == KIND_NOTIFY) {
        notice = w;
        if (!XGetWindowAttributes(display, w, &attributes)) {
            return;
        }
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
    int width;
    int height;

    if (i != MAX_CLIENTS) {
        width = clients[i].width;
        height = clients[i].height;
        if (request.value_mask & CWWidth) {
            width = request.width;
            if (width < MIN_WIDTH) {
                width = MIN_WIDTH;
            }
            clients[i].maximized = 0;
        }
        if (request.value_mask & CWHeight) {
            height = request.height;
            if (height < MIN_HEIGHT) {
                height = MIN_HEIGHT;
            }
            height = height + TITLE_HEIGHT;
            clients[i].maximized = 0;
        }
        set_geometry(i, clients[i].x, clients[i].y, width, height);
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
        desktop_focused = 0;
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
    } else if (symbol == XK_m) {
        toggle_maximize(active);
    } else if (symbol == XK_q || symbol == XK_F4) {
        close_client(active);
    } else if (symbol == XK_Tab) {
        cycle();
    } else if (symbol == XK_e && shift) {
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
    settle(i);
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
        if (press.x >= subtract(clients[i].awidth, BUTTON_WIDTH)) {
            close_client(i);
        } else if (last_click_index == i && press.time <= last_click_time + DOUBLE_CLICK_MS) {
            last_click_index = MAX_CLIENTS;
            toggle_maximize(i);
        } else {
            last_click_index = i;
            last_click_time = press.time;
            start_drag(i, DRAG_MOVE, press);
        }
    }
}

static void on_motion(XMotionEvent motion) {
    int dx;
    int dy;
    int width;
    int height;

    if (drag_mode == DRAG_NONE || !clients[drag_index].used) {
        return;
    }
    dx = subtract(motion.x_root, drag_pointer_x);
    dy = subtract(motion.y_root, drag_pointer_y);
    if (dx != 0 || dy != 0) {
        clients[drag_index].maximized = 0;
    }
    if (drag_mode == DRAG_MOVE) {
        int x = drag_x + dx;
        int y = drag_y + dy;

        if (y < PANEL_HEIGHT) {
            y = PANEL_HEIGHT;
        }
        set_geometry(drag_index, x, y, clients[drag_index].width, clients[drag_index].height);
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
    set_geometry(drag_index, clients[drag_index].x, clients[drag_index].y, width, height);
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
    KeySym symbols[KEY_COUNT] = {XK_Return, XK_f, XK_s, XK_q, XK_Tab, XK_e,
                                 XK_1, XK_2, XK_3, XK_4, XK_F4, XK_m};
    unsigned int variants[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    unsigned int shifts[2] = {0, ShiftMask};
    unsigned int buttons[2] = {Button1, Button3};
    int i;
    int j;
    int k;

    for (i = 0; i < KEY_COUNT; i++) {
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
    Atom supported[8];
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
    fcntl(connection, F_SETFD, FD_CLOEXEC);
    screen_number = DefaultScreen(display);
    root = RootWindow(display, screen_number);
    screen_width = DisplayWidth(display, screen_number);
    screen_height = DisplayHeight(display, screen_number);
    body_height = subtract(screen_height, PANEL_HEIGHT);

    XSetErrorHandler(startup_error);
    XSelectInput(display, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(display, False);
    if (other_wm) {
        fprintf(stderr, "another window manager is already running\n");
        XCloseDisplay(display);
        return 1;
    }
    XSetErrorHandler(ignore_error);

    atom_supported = XInternAtom(display, "_NET_SUPPORTED", False);
    atom_client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    atom_active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    atom_current_desktop = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
    atom_number_of_desktops = XInternAtom(display, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_close_window = XInternAtom(display, "_NET_CLOSE_WINDOW", False);
    atom_workarea = XInternAtom(display, "_NET_WORKAREA", False);
    atom_extents = XInternAtom(display, "_NET_FRAME_EXTENTS", False);
    atom_check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    atom_name = XInternAtom(display, "_NET_WM_NAME", False);
    atom_utf8 = XInternAtom(display, "UTF8_STRING", False);
    atom_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    atom_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);

    supported[0] = atom_client_list;
    supported[1] = atom_active_window;
    supported[2] = atom_current_desktop;
    supported[3] = atom_number_of_desktops;
    supported[4] = atom_close_window;
    supported[5] = atom_workarea;
    supported[6] = atom_extents;
    supported[7] = atom_check;
    XChangeProperty(display, root, atom_supported, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported, 8);

    gc = XCreateGC(display, root, 0, NULL);
    XSetForeground(display, gc, BlackPixel(display, screen_number));
    XDefineCursor(display, root, XCreateFontCursor(display, XC_left_ptr));
    bind_keys();
    publish_static();
    publish();

    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    setenv("DE_SESSION_PID", pid, 1);
    spawn("./desktop");
    spawn("./panel");
    spawn("./notify");

    while (running) {
        tick_animations();
        if (XPending(display) == 0) {
            FD_ZERO(&fds);
            FD_SET(connection, &fds);
            interval.tv_sec = any_animating() ? 0 : 1;
            interval.tv_usec = any_animating() ? FRAME_MS * 1000 : 0;
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
            while (XCheckTypedEvent(display, MotionNotify, &event)) {
            }
            on_motion(event.xmotion);
            break;
        case Expose:
            i = find_by_frame(event.xexpose.window);
            if (i != MAX_CLIENTS && event.xexpose.count == 0) {
                draw_frame(i);
            }
            break;
        case EnterNotify:
            if (event.xcrossing.mode == NotifyNormal && drag_mode == DRAG_NONE) {
                i = find_by_frame(event.xcrossing.window);
                if (i != MAX_CLIENTS && i != active) {
                    focus_client(i, 0);
                } else if (i == MAX_CLIENTS && event.xcrossing.window == desktop) {
                    focus_desktop();
                }
            }
            break;
        case PropertyNotify:
            i = find_by_client(event.xproperty.window);
            if (i != MAX_CLIENTS &&
                (event.xproperty.atom == atom_name || event.xproperty.atom == XA_WM_NAME)) {
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