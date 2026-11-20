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

/*
 * Tiling window manager inspired by Hyprland (dwindle layout) and
 * Niri (scrollable columns). Written without the minus character on purpose.
 *
 * Keys (MOD is Super by default, change it below to Mod1Mask for Alt):
 *   MOD+Return        terminal            MOD+E        files
 *   MOD+S             settings            MOD+Q        close window
 *   MOD+Shift+E       quit                MOD+Tab      cycle focus
 *   MOD+Space         toggle dwindle / scrolling layout
 *   MOD+V             toggle floating     MOD+F        fullscreen
 *   MOD+R             cycle column width presets (scrolling) or reset split ratio
 *   MOD+C             toggle always center focused column (scrolling)
 *   MOD+H/J/K/L       focus left/down/up/right (arrow keys work too)
 *   MOD+Shift+HJKL    move window (swap with neighbor, or nudge when floating)
 *   MOD+Ctrl+HJKL     resize split ratio / column width / floating size
 *   MOD+1..4          switch workspace    MOD+Shift+1..4  send window to workspace
 *   MOD+Wheel         previous / next workspace
 *   MOD+Left drag     move (tiled window becomes floating)
 *   MOD+Right drag    resize (tiled window becomes floating)
 */

#define MOD Mod4Mask

#define PANEL_HEIGHT 32
#define MAX_CLIENTS 64
#define WORKSPACES 4
#define MIN_WIDTH 120
#define MIN_HEIGHT 60
#define MARGIN 12
#define BORDER 2
#define GAP_IN 6
#define GAP_OUT 12
#define ANIMATION_MS 220
#define FRAME_MS 16
#define OPEN_SHRINK 8
#define RATIO_DEFAULT 500
#define RATIO_STEP 50
#define COLW_DEFAULT 500
#define FLOAT_STEP 40
#define ACTIVE_COLOR "#33ccff"
#define INACTIVE_COLOR "#595959"

enum { KIND_NORMAL, KIND_PANEL, KIND_DESKTOP, KIND_NOTIFY };
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
enum { LAYOUT_DWINDLE, LAYOUT_SCROLL };
enum { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN };

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

typedef struct {
    int used;
    Window window;
    int workspace;
    int seq;
    int floating;
    int fullscreen;
    int mapped;
    int ignore_unmap;
    int bw;
    int fx;
    int fy;
    int fw;
    int fh;
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
} Client;

static Display *display;
static Window root;
static Client clients[MAX_CLIENTS];
static int active = MAX_CLIENTS;
static int current = 0;
static int next_seq = 1;
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
static long last_tick = 0;
static unsigned long color_active;
static unsigned long color_inactive;
static int center_mode = 0;
static int ws_layout[WORKSPACES];
static int ws_ratio[WORKSPACES];
static int ws_colw[WORKSPACES];
static int ws_scroll[WORKSPACES];
static int ws_last[WORKSPACES];
static Atom atom_supported;
static Atom atom_client_list;
static Atom atom_active_window;
static Atom atom_current_desktop;
static Atom atom_number_of_desktops;
static Atom atom_close_window;
static Atom atom_workarea;
static Atom atom_check;
static Atom atom_name;
static Atom atom_utf8;
static Atom atom_protocols;
static Atom atom_delete;
static Atom atom_state;
static Atom atom_fullscreen;

static int subtract(int a, int b) {
    return a + (~b + 1);
}

static long subtract_long(long a, long b) {
    return a + (~b + 1);
}

static int iabs(int a) {
    return a < 0 ? subtract(0, a) : a;
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

static unsigned long get_color(const char *name, unsigned long fallback) {
    XColor color;
    Colormap map = DefaultColormap(display, screen_number);

    if (XParseColor(display, map, name, &color) && XAllocColor(display, map, &color)) {
        return color.pixel;
    }
    return fallback;
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
        if (clients[i].used && clients[i].window == w) {
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

static int pick_focus(void) {
    int last = ws_last[current];

    if (last != MAX_CLIENTS && clients[last].used && clients[last].workspace == current) {
        return last;
    }
    return first_in_workspace();
}

static int has_fullscreen(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].workspace == current && clients[i].fullscreen) {
            return 1;
        }
    }
    return 0;
}

/* Fills out with tiled clients of a workspace, sorted by creation order. */
static int tiled_list(int ws, int *out) {
    int n = 0;
    int i;
    int j;
    int tmp;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].workspace == ws &&
            !clients[i].floating && !clients[i].fullscreen) {
            out[n] = i;
            n++;
        }
    }
    for (i = 1; i < n; i++) {
        tmp = out[i];
        j = i;
        while (j > 0 && clients[out[subtract(j, 1)]].seq > clients[tmp].seq) {
            out[j] = out[subtract(j, 1)];
            j = subtract(j, 1);
        }
        out[j] = tmp;
    }
    return n;
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

static void publish(void) {
    unsigned long list[MAX_CLIENTS];
    unsigned long count = 0;
    unsigned long number = WORKSPACES;
    unsigned long desktop_value = (unsigned long)current;
    unsigned long active_value = 0;
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used) {
            list[count] = clients[i].window;
            count++;
        }
    }
    if (active != MAX_CLIENTS) {
        active_value = clients[active].window;
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

static void send_configure(int i) {
    XConfigureEvent configure;
    int bw = clients[i].bw;

    memset(&configure, 0, sizeof(configure));
    configure.type = ConfigureNotify;
    configure.display = display;
    configure.event = clients[i].window;
    configure.window = clients[i].window;
    configure.x = clients[i].ax + bw;
    configure.y = clients[i].ay + bw;
    configure.width = subtract(clients[i].awidth, 2 * bw);
    configure.height = subtract(clients[i].aheight, 2 * bw);
    configure.border_width = bw;
    configure.above = None;
    configure.override_redirect = False;
    XSendEvent(display, clients[i].window, False, StructureNotifyMask, (XEvent *)&configure);
}

/* Moves the real window. Rect is the outer rectangle including the border. */
static void place(int i, int x, int y, int width, int height) {
    int bw = clients[i].fullscreen ? 0 : BORDER;
    int inner_w;
    int inner_h;
    int limit_x = screen_width * 3;
    int limit_y = screen_height * 3;

    if (x < subtract(0, limit_x)) {
        x = subtract(0, limit_x);
    }
    if (x > limit_x) {
        x = limit_x;
    }
    if (y < subtract(0, limit_y)) {
        y = subtract(0, limit_y);
    }
    if (y > limit_y) {
        y = limit_y;
    }
    inner_w = subtract(width, 2 * bw);
    inner_h = subtract(height, 2 * bw);
    if (inner_w < 1) {
        inner_w = 1;
    }
    if (inner_h < 1) {
        inner_h = 1;
    }
    if (bw != clients[i].bw) {
        XSetWindowBorderWidth(display, clients[i].window, (unsigned int)bw);
        clients[i].bw = bw;
    }
    clients[i].ax = x;
    clients[i].ay = y;
    clients[i].awidth = width;
    clients[i].aheight = height;
    XMoveResizeWindow(display, clients[i].window, x, y,
                      (unsigned int)inner_w, (unsigned int)inner_h);
}

static void set_geometry(int i, int x, int y, int width, int height) {
    clients[i].animating = 0;
    if (clients[i].floating) {
        clients[i].fx = x;
        clients[i].fy = y;
        clients[i].fw = width;
        clients[i].fh = height;
    }
    clients[i].x = x;
    clients[i].y = y;
    clients[i].width = width;
    clients[i].height = height;
    place(i, x, y, width, height);
}

static void animate_to(int i, int x, int y, int width, int height) {
    if (clients[i].animating && clients[i].x == x && clients[i].y == y &&
        clients[i].width == width && clients[i].height == height) {
        return;
    }
    clients[i].x = x;
    clients[i].y = y;
    clients[i].width = width;
    clients[i].height = height;
    if (!clients[i].animating && clients[i].ax == x && clients[i].ay == y &&
        clients[i].awidth == width && clients[i].aheight == height) {
        return;
    }
    clients[i].from_x = clients[i].ax;
    clients[i].from_y = clients[i].ay;
    clients[i].from_width = clients[i].awidth;
    clients[i].from_height = clients[i].aheight;
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

static void set_visible(int i, int visible) {
    if (visible && !clients[i].mapped) {
        XMapWindow(display, clients[i].window);
        clients[i].mapped = 1;
    } else if (!visible && clients[i].mapped) {
        clients[i].ignore_unmap++;
        XUnmapWindow(display, clients[i].window);
        clients[i].mapped = 0;
    }
}

static void update_borders(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used) {
            XSetWindowBorder(display, clients[i].window,
                             i == active ? color_active : color_inactive);
        }
    }
}

static void raise_chrome(void) {
    if (has_fullscreen()) {
        return;
    }
    if (panel != 0) {
        XRaiseWindow(display, panel);
    }
    if (notice != 0) {
        XRaiseWindow(display, notice);
    }
}

static void raise_client(int i) {
    if (clients[i].floating || clients[i].fullscreen) {
        XRaiseWindow(display, clients[i].window);
    }
    raise_chrome();
}

/* Floating windows above tiled ones, fullscreen above everything, panel on top otherwise. */
static void restack(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].workspace == current &&
            clients[i].floating && !clients[i].fullscreen) {
            XRaiseWindow(display, clients[i].window);
        }
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].workspace == current && clients[i].fullscreen) {
            XRaiseWindow(display, clients[i].window);
        }
    }
    raise_chrome();
}

static void tile_to(int i, Rect r) {
    animate_to(i, r.x, r.y, r.w, r.h);
}

static void arrange(void) {
    int list[MAX_CLIENTS];
    int n = tiled_list(current, list);
    int k;
    int i;
    int step;
    int cut;
    Rect area;
    Rect r;
    Rect cell;

    area.x = GAP_OUT;
    area.y = PANEL_HEIGHT + GAP_OUT;
    area.w = subtract(screen_width, 2 * GAP_OUT);
    area.h = subtract(body_height, 2 * GAP_OUT);

    if (ws_layout[current] == LAYOUT_DWINDLE) {
        /* Hyprland dwindle: each window takes a share, the rest recurses into the remainder. */
        r = area;
        for (k = 0; k < n; k++) {
            if (k == subtract(n, 1)) {
                tile_to(list[k], r);
                break;
            }
            cell = r;
            if (r.w >= r.h) {
                cut = subtract(r.w, GAP_IN) * ws_ratio[current] / 1000;
                cell.w = cut;
                r.x = r.x + cut + GAP_IN;
                r.w = subtract(r.w, cut + GAP_IN);
            } else {
                cut = subtract(r.h, GAP_IN) * ws_ratio[current] / 1000;
                cell.h = cut;
                r.y = r.y + cut + GAP_IN;
                r.h = subtract(r.h, cut + GAP_IN);
            }
            tile_to(list[k], cell);
        }
    } else {
        /* Niri style: infinite strip of columns, the viewport scrolls to the focused one. */
        int colw = area.w * ws_colw[current] / 1000;
        int total;
        int left;
        int scroll = ws_scroll[current];
        int anchor = n;
        int max_scroll;
        int want = active;

        if (colw < MIN_WIDTH) {
            colw = MIN_WIDTH;
        }
        step = colw + GAP_IN;
        total = subtract(n * step, GAP_IN);
        for (k = 0; k < n; k++) {
            if (list[k] == want) {
                anchor = k;
            }
        }
        if (anchor == n && ws_last[current] != MAX_CLIENTS) {
            for (k = 0; k < n; k++) {
                if (list[k] == ws_last[current]) {
                    anchor = k;
                }
            }
        }
        if (anchor != n) {
            left = anchor * step;
            if (center_mode) {
                scroll = subtract(left, subtract(area.w, colw) / 2);
            } else if (left < scroll) {
                scroll = left;
            } else if (left + colw > scroll + area.w) {
                scroll = subtract(left + colw, area.w);
            }
        }
        if (!center_mode) {
            max_scroll = subtract(total, area.w);
            if (max_scroll < 0) {
                max_scroll = 0;
            }
            if (scroll > max_scroll) {
                scroll = max_scroll;
            }
            if (scroll < 0) {
                scroll = 0;
            }
        }
        ws_scroll[current] = scroll;
        for (k = 0; k < n; k++) {
            cell.x = subtract(area.x + k * step, scroll);
            cell.y = area.y;
            cell.w = colw;
            cell.h = area.h;
            tile_to(list[k], cell);
        }
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used || clients[i].workspace != current) {
            continue;
        }
        if (clients[i].fullscreen) {
            animate_to(i, 0, 0, screen_width, screen_height);
        } else if (clients[i].floating) {
            animate_to(i, clients[i].fx, clients[i].fy, clients[i].fw, clients[i].fh);
        }
    }
}

static void focus_client(int i, int raise) {
    if (i == MAX_CLIENTS) {
        active = MAX_CLIENTS;
        desktop_focused = 0;
        XSetInputFocus(display, root, RevertToPointerRoot, CurrentTime);
        update_borders();
        publish();
        return;
    }
    desktop_focused = 0;
    active = i;
    ws_last[clients[i].workspace] = i;
    if (raise) {
        raise_client(i);
    }
    XSetInputFocus(display, clients[i].window, RevertToPointerRoot, CurrentTime);
    update_borders();
    publish();
    if (ws_layout[current] == LAYOUT_SCROLL && clients[i].workspace == current) {
        arrange();
    }
}

static void focus_desktop(void) {
    if (desktop == 0 || desktop_focused) {
        return;
    }
    desktop_focused = 1;
    active = MAX_CLIENTS;
    XSetInputFocus(display, desktop, RevertToPointerRoot, CurrentTime);
    update_borders();
    publish();
}

static void warp_to(int i) {
    XWarpPointer(display, None, root, 0, 0, 0, 0,
                 clients[i].x + clients[i].width / 2, clients[i].y + clients[i].height / 2);
}

static void switch_workspace(int n) {
    int i;
    int offset;

    if (n < 0 || n >= WORKSPACES || n == current) {
        return;
    }
    offset = n > current ? screen_width : subtract(0, screen_width);
    current = n;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            continue;
        }
        if (clients[i].workspace == current) {
            /* Slide the incoming windows in from the side, like Hyprland workspace slide. */
            clients[i].animating = 0;
            place(i, clients[i].ax + offset, clients[i].ay, clients[i].awidth, clients[i].aheight);
            set_visible(i, 1);
        } else {
            set_visible(i, 0);
        }
    }
    arrange();
    focus_client(pick_focus(), 1);
    restack();
}

static void send_to_workspace(int i, int n) {
    if (i == MAX_CLIENTS || n < 0 || n >= WORKSPACES || n == clients[i].workspace) {
        return;
    }
    clients[i].workspace = n;
    set_visible(i, 0);
    if (active == i) {
        active = MAX_CLIENTS;
    }
    arrange();
    focus_client(pick_focus(), 1);
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
    if (XGetWMProtocols(display, clients[i].window, &protocols, &count)) {
        for (n = 0; n < count; n++) {
            if (protocols[n] == atom_delete) {
                supported = 1;
            }
        }
        XFree(protocols);
    }
    if (!supported) {
        XKillClient(display, clients[i].window);
        return;
    }
    memset(&message, 0, sizeof(message));
    message.xclient.type = ClientMessage;
    message.xclient.window = clients[i].window;
    message.xclient.message_type = atom_protocols;
    message.xclient.format = 32;
    message.xclient.data.l[0] = (long)atom_delete;
    message.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, clients[i].window, False, NoEventMask, &message);
    XFlush(display);
}

static int neighbor(int i, int dir) {
    int list[MAX_CLIENTS];
    int n;
    int k;
    int j;
    int best = MAX_CLIENTS;
    int best_score = 0;
    int cx;
    int cy;
    int dx;
    int dy;
    int primary;
    int cross;
    int score;

    if (ws_layout[current] == LAYOUT_SCROLL && !clients[i].floating) {
        n = tiled_list(current, list);
        for (k = 0; k < n; k++) {
            if (list[k] != i) {
                continue;
            }
            if (dir == DIR_LEFT && k > 0) {
                return list[subtract(k, 1)];
            }
            if (dir == DIR_RIGHT && k + 1 < n) {
                return list[k + 1];
            }
        }
        return MAX_CLIENTS;
    }
    cx = clients[i].x + clients[i].width / 2;
    cy = clients[i].y + clients[i].height / 2;
    for (j = 0; j < MAX_CLIENTS; j++) {
        if (!clients[j].used || j == i || clients[j].workspace != current || clients[j].fullscreen) {
            continue;
        }
        dx = subtract(clients[j].x + clients[j].width / 2, cx);
        dy = subtract(clients[j].y + clients[j].height / 2, cy);
        if (dir == DIR_LEFT && dx < 0) {
            primary = iabs(dx);
            cross = iabs(dy);
        } else if (dir == DIR_RIGHT && dx > 0) {
            primary = dx;
            cross = iabs(dy);
        } else if (dir == DIR_UP && dy < 0) {
            primary = iabs(dy);
            cross = iabs(dx);
        } else if (dir == DIR_DOWN && dy > 0) {
            primary = dy;
            cross = iabs(dx);
        } else {
            continue;
        }
        score = primary + 2 * cross;
        if (best == MAX_CLIENTS || score < best_score) {
            best = j;
            best_score = score;
        }
    }
    return best;
}

static void focus_dir(int dir) {
    int j;

    if (active == MAX_CLIENTS) {
        j = pick_focus();
    } else {
        j = neighbor(active, dir);
    }
    if (j != MAX_CLIENTS) {
        focus_client(j, 1);
        warp_to(j);
    }
}

static void move_dir(int dir) {
    int j;
    int tmp;

    if (active == MAX_CLIENTS || clients[active].fullscreen) {
        return;
    }
    if (clients[active].floating) {
        if (dir == DIR_LEFT) {
            clients[active].fx = subtract(clients[active].fx, FLOAT_STEP);
        } else if (dir == DIR_RIGHT) {
            clients[active].fx = clients[active].fx + FLOAT_STEP;
        } else if (dir == DIR_UP) {
            clients[active].fy = subtract(clients[active].fy, FLOAT_STEP);
        } else {
            clients[active].fy = clients[active].fy + FLOAT_STEP;
        }
        if (clients[active].fy < PANEL_HEIGHT) {
            clients[active].fy = PANEL_HEIGHT;
        }
        arrange();
        return;
    }
    j = neighbor(active, dir);
    if (j == MAX_CLIENTS || clients[j].floating) {
        return;
    }
    tmp = clients[active].seq;
    clients[active].seq = clients[j].seq;
    clients[j].seq = tmp;
    arrange();
}

static void resize_step(int dir) {
    int i = active;
    int sign = (dir == DIR_RIGHT || dir == DIR_DOWN) ? 1 : subtract(0, 1);

    if (i != MAX_CLIENTS && clients[i].floating && !clients[i].fullscreen) {
        if (dir == DIR_LEFT || dir == DIR_RIGHT) {
            clients[i].fw = clients[i].fw + sign * FLOAT_STEP;
            if (clients[i].fw < MIN_WIDTH) {
                clients[i].fw = MIN_WIDTH;
            }
        } else {
            clients[i].fh = clients[i].fh + sign * FLOAT_STEP;
            if (clients[i].fh < MIN_HEIGHT) {
                clients[i].fh = MIN_HEIGHT;
            }
        }
    } else if (ws_layout[current] == LAYOUT_DWINDLE) {
        ws_ratio[current] = ws_ratio[current] + sign * RATIO_STEP;
        if (ws_ratio[current] < 150) {
            ws_ratio[current] = 150;
        }
        if (ws_ratio[current] > 850) {
            ws_ratio[current] = 850;
        }
    } else {
        ws_colw[current] = ws_colw[current] + sign * RATIO_STEP;
        if (ws_colw[current] < 200) {
            ws_colw[current] = 200;
        }
        if (ws_colw[current] > 1000) {
            ws_colw[current] = 1000;
        }
    }
    arrange();
}

static void cycle_colw(void) {
    static const int presets[4] = {333, 500, 667, 1000};
    int k;

    if (ws_layout[current] == LAYOUT_DWINDLE) {
        ws_ratio[current] = RATIO_DEFAULT;
        arrange();
        return;
    }
    for (k = 0; k < 4; k++) {
        if (ws_colw[current] < presets[k]) {
            ws_colw[current] = presets[k];
            arrange();
            return;
        }
    }
    ws_colw[current] = presets[0];
    arrange();
}

static void toggle_layout(void) {
    ws_layout[current] = ws_layout[current] == LAYOUT_DWINDLE ? LAYOUT_SCROLL : LAYOUT_DWINDLE;
    ws_scroll[current] = 0;
    arrange();
}

static void toggle_floating(int i) {
    if (i == MAX_CLIENTS || clients[i].fullscreen) {
        return;
    }
    if (!clients[i].floating) {
        clients[i].floating = 1;
        clients[i].fw = screen_width * 6 / 10;
        clients[i].fh = body_height * 6 / 10;
        clients[i].fx = subtract(screen_width, clients[i].fw) / 2;
        clients[i].fy = PANEL_HEIGHT + subtract(body_height, clients[i].fh) / 2;
    } else {
        clients[i].floating = 0;
    }
    arrange();
    restack();
    raise_client(i);
}

static void set_fullscreen(int i, int mode) {
    int on;
    unsigned long value = (unsigned long)atom_fullscreen;

    if (i == MAX_CLIENTS) {
        return;
    }
    on = mode == 2 ? !clients[i].fullscreen : mode;
    if (on == clients[i].fullscreen) {
        return;
    }
    clients[i].fullscreen = on;
    XChangeProperty(display, clients[i].window, atom_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&value, on ? 1 : 0);
    place(i, clients[i].ax, clients[i].ay, clients[i].awidth, clients[i].aheight);
    arrange();
    restack();
    raise_client(i);
}

static void cycle(void) {
    int start = active;
    int step;
    int i;

    for (step = 1; step <= MAX_CLIENTS; step++) {
        i = (start + step) % MAX_CLIENTS;
        if (clients[i].used && clients[i].workspace == current && i != active) {
            focus_client(i, 1);
            warp_to(i);
            return;
        }
    }
}

static void manage_window(Window w) {
    XWindowAttributes attributes;
    XSizeHints hints;
    long supplied;
    Window transient = None;
    int i;
    int j;
    int new_seq;
    int width;
    int height;

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

    /* Niri opens new columns right of the focused one, dwindle simply appends. */
    new_seq = next_seq;
    if (ws_layout[current] == LAYOUT_SCROLL && active != MAX_CLIENTS &&
        clients[active].workspace == current && !clients[active].floating) {
        new_seq = clients[active].seq + 1;
        for (j = 0; j < MAX_CLIENTS; j++) {
            if (clients[j].used && clients[j].seq >= new_seq) {
                clients[j].seq++;
            }
        }
    }
    next_seq++;

    width = attributes.width + 2 * BORDER;
    height = attributes.height + 2 * BORDER;
    if (width < MIN_WIDTH) {
        width = MIN_WIDTH;
    }
    if (height < MIN_HEIGHT) {
        height = MIN_HEIGHT;
    }
    if (width > screen_width) {
        width = screen_width;
    }
    if (height > body_height) {
        height = body_height;
    }

    memset(&clients[i], 0, sizeof(clients[i]));
    clients[i].used = 1;
    clients[i].window = w;
    clients[i].workspace = current;
    clients[i].seq = new_seq;
    clients[i].bw = BORDER;
    clients[i].fw = width;
    clients[i].fh = height;
    clients[i].fx = subtract(screen_width, width) / 2;
    clients[i].fy = PANEL_HEIGHT + subtract(body_height, height) / 2;
    if (XGetTransientForHint(display, w, &transient)) {
        clients[i].floating = 1;
    }
    if (XGetWMNormalHints(display, w, &hints, &supplied) &&
        (hints.flags & PMinSize) && (hints.flags & PMaxSize) &&
        hints.min_width == hints.max_width && hints.min_height == hints.max_height) {
        clients[i].floating = 1;
    }
    clients[i].ax = screen_width / 2;
    clients[i].ay = screen_height / 2;
    clients[i].awidth = 1;
    clients[i].aheight = 1;

    XSelectInput(display, w, EnterWindowMask);
    XSetWindowBorderWidth(display, w, BORDER);
    XSetWindowBorder(display, w, color_inactive);

    arrange();
    if (clients[i].animating) {
        /* Pop in: start slightly smaller than the final rectangle. */
        clients[i].from_x = clients[i].x + clients[i].width * OPEN_SHRINK / 200;
        clients[i].from_y = clients[i].y + clients[i].height * OPEN_SHRINK / 200;
        clients[i].from_width = clients[i].width * subtract(100, OPEN_SHRINK) / 100;
        clients[i].from_height = clients[i].height * subtract(100, OPEN_SHRINK) / 100;
        place(i, clients[i].from_x, clients[i].from_y,
              clients[i].from_width, clients[i].from_height);
    }
    set_visible(i, 1);
    restack();
    focus_client(i, 1);
}

static void remove_client(int i) {
    int was_current = clients[i].workspace == current;
    int k;

    clients[i].used = 0;
    clients[i].animating = 0;
    for (k = 0; k < WORKSPACES; k++) {
        if (ws_last[k] == i) {
            ws_last[k] = MAX_CLIENTS;
        }
    }
    if (drag_mode != DRAG_NONE && drag_index == i) {
        drag_mode = DRAG_NONE;
        XUngrabPointer(display, CurrentTime);
    }
    if (active == i) {
        active = MAX_CLIENTS;
        focus_client(pick_focus(), 1);
    } else {
        publish();
    }
    if (was_current) {
        arrange();
    }
}

static void on_map_request(Window w) {
    XWindowAttributes attributes;
    int kind;
    int i = find_by_client(w);

    if (i != MAX_CLIENTS) {
        if (clients[i].workspace == current) {
            set_visible(i, 1);
        }
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
    manage_window(w);
}

static void on_configure_request(XConfigureRequestEvent request) {
    XWindowChanges changes;
    int i = find_by_client(request.window);

    if (i != MAX_CLIENTS) {
        if (clients[i].floating && !clients[i].fullscreen) {
            if (request.value_mask & CWWidth) {
                clients[i].fw = request.width + 2 * BORDER;
                if (clients[i].fw < MIN_WIDTH) {
                    clients[i].fw = MIN_WIDTH;
                }
            }
            if (request.value_mask & CWHeight) {
                clients[i].fh = request.height + 2 * BORDER;
                if (clients[i].fh < MIN_HEIGHT) {
                    clients[i].fh = MIN_HEIGHT;
                }
            }
            arrange();
        } else {
            send_configure(i);
        }
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

    if (i == MAX_CLIENTS) {
        return;
    }
    if (clients[i].ignore_unmap > 0) {
        clients[i].ignore_unmap--;
        return;
    }
    remove_client(i);
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
        remove_client(i);
    }
}

static void on_key(XKeyEvent key) {
    KeySym symbol;
    int shift = (key.state & ShiftMask) != 0;
    int ctrl = (key.state & ControlMask) != 0;
    int dir = DIR_LEFT;
    int is_dir = 0;
    int number;

    if ((key.state & MOD) == 0) {
        return;
    }
    symbol = XLookupKeysym(&key, 0);
    if (symbol == XK_h || symbol == XK_Left) {
        dir = DIR_LEFT;
        is_dir = 1;
    } else if (symbol == XK_l || symbol == XK_Right) {
        dir = DIR_RIGHT;
        is_dir = 1;
    } else if (symbol == XK_k || symbol == XK_Up) {
        dir = DIR_UP;
        is_dir = 1;
    } else if (symbol == XK_j || symbol == XK_Down) {
        dir = DIR_DOWN;
        is_dir = 1;
    }
    if (is_dir) {
        if (ctrl) {
            resize_step(dir);
        } else if (shift) {
            move_dir(dir);
        } else {
            focus_dir(dir);
        }
    } else if (symbol == XK_Return) {
        spawn("./terminal");
    } else if (symbol == XK_e) {
        if (shift) {
            running = 0;
        } else {
            spawn("./files");
        }
    } else if (symbol == XK_s) {
        spawn("./settings");
    } else if (symbol == XK_q) {
        close_client(active);
    } else if (symbol == XK_Tab) {
        cycle();
    } else if (symbol == XK_space) {
        toggle_layout();
    } else if (symbol == XK_v) {
        toggle_floating(active);
    } else if (symbol == XK_f) {
        set_fullscreen(active, 2);
    } else if (symbol == XK_r) {
        cycle_colw();
    } else if (symbol == XK_c) {
        center_mode = !center_mode;
        arrange();
    } else if (symbol >= XK_1 && symbol < XK_1 + WORKSPACES) {
        number = subtract((int)symbol, (int)XK_1);
        if (shift) {
            send_to_workspace(active, number);
        } else {
            switch_workspace(number);
        }
    }
}

static void start_drag(int i, int mode, XButtonEvent press) {
    settle(i);
    if (!clients[i].floating) {
        clients[i].floating = 1;
        clients[i].fx = clients[i].ax;
        clients[i].fy = clients[i].ay;
        clients[i].fw = clients[i].awidth;
        clients[i].fh = clients[i].aheight;
        arrange();
        restack();
        raise_client(i);
    }
    drag_mode = mode;
    drag_index = i;
    drag_pointer_x = press.x_root;
    drag_pointer_y = press.y_root;
    drag_x = clients[i].fx;
    drag_y = clients[i].fy;
    drag_width = clients[i].fw;
    drag_height = clients[i].fh;
    XGrabPointer(display, root, False, ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

static void on_button_press(XButtonEvent press) {
    int i;

    if ((press.state & MOD) == 0) {
        return;
    }
    if (press.button == Button4) {
        switch_workspace(subtract(current, 1));
        return;
    }
    if (press.button == Button5) {
        switch_workspace(current + 1);
        return;
    }
    i = find_by_client(press.subwindow);
    if (i == MAX_CLIENTS) {
        return;
    }
    focus_client(i, 1);
    if (clients[i].fullscreen) {
        return;
    }
    if (press.button == Button1) {
        start_drag(i, DRAG_MOVE, press);
    } else if (press.button == Button3) {
        start_drag(i, DRAG_RESIZE, press);
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
        set_geometry(drag_index, x, y, clients[drag_index].fw, clients[drag_index].fh);
        return;
    }
    width = drag_width + dx;
    height = drag_height + dy;
    if (width < MIN_WIDTH) {
        width = MIN_WIDTH;
    }
    if (height < MIN_HEIGHT) {
        height = MIN_HEIGHT;
    }
    set_geometry(drag_index, clients[drag_index].fx, clients[drag_index].fy, width, height);
}

static void on_client_message(XClientMessageEvent message) {
    int i;
    int action;

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
    } else if (message.message_type == atom_state) {
        i = find_by_client(message.window);
        if (i == MAX_CLIENTS) {
            return;
        }
        if ((Atom)message.data.l[1] == atom_fullscreen ||
            (Atom)message.data.l[2] == atom_fullscreen) {
            action = (int)message.data.l[0];
            set_fullscreen(i, action == 0 ? 0 : (action == 1 ? 1 : 2));
        }
    }
}

static void bind_keys(void) {
    KeySym symbols[22] = {XK_Return, XK_e, XK_s, XK_q, XK_Tab, XK_space, XK_v, XK_f,
                          XK_r, XK_c, XK_h, XK_j, XK_k, XK_l, XK_Left, XK_Down,
                          XK_Up, XK_Right, XK_1, XK_2, XK_3, XK_4};
    unsigned int variants[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    unsigned int shifts[2] = {0, ShiftMask};
    unsigned int ctrls[2] = {0, ControlMask};
    unsigned int buttons[4] = {Button1, Button3, Button4, Button5};
    int i;
    int j;
    int k;
    int m;

    for (i = 0; i < 22; i++) {
        KeyCode code = XKeysymToKeycode(display, symbols[i]);
        if (code == 0) {
            continue;
        }
        for (j = 0; j < 4; j++) {
            for (k = 0; k < 2; k++) {
                for (m = 0; m < 2; m++) {
                    XGrabKey(display, code, MOD | variants[j] | shifts[k] | ctrls[m], root, True,
                             GrabModeAsync, GrabModeAsync);
                }
            }
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            XGrabButton(display, buttons[i], MOD | variants[j], root, True,
                        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                        GrabModeAsync, GrabModeAsync, None, None);
        }
    }
}

int main(void) {
    XEvent event;
    fd_set fds;
    struct timeval interval;
    Atom supported[9];
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
    atom_check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    atom_name = XInternAtom(display, "_NET_WM_NAME", False);
    atom_utf8 = XInternAtom(display, "UTF8_STRING", False);
    atom_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    atom_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    atom_state = XInternAtom(display, "_NET_WM_STATE", False);
    atom_fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);

    supported[0] = atom_client_list;
    supported[1] = atom_active_window;
    supported[2] = atom_current_desktop;
    supported[3] = atom_number_of_desktops;
    supported[4] = atom_close_window;
    supported[5] = atom_workarea;
    supported[6] = atom_check;
    supported[7] = atom_state;
    supported[8] = atom_fullscreen;
    XChangeProperty(display, root, atom_supported, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported, 9);

    color_active = get_color(ACTIVE_COLOR, WhitePixel(display, screen_number));
    color_inactive = get_color(INACTIVE_COLOR, BlackPixel(display, screen_number));
    for (i = 0; i < WORKSPACES; i++) {
        ws_layout[i] = LAYOUT_DWINDLE;
        ws_ratio[i] = RATIO_DEFAULT;
        ws_colw[i] = COLW_DEFAULT;
        ws_scroll[i] = 0;
        ws_last[i] = MAX_CLIENTS;
    }

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
        case EnterNotify:
            /* Focus follows mouse, but not while windows slide under a still pointer. */
            if (event.xcrossing.mode == NotifyNormal && drag_mode == DRAG_NONE &&
                !any_animating()) {
                i = find_by_client(event.xcrossing.window);
                if (i != MAX_CLIENTS && i != active && clients[i].mapped) {
                    focus_client(i, 0);
                } else if (i == MAX_CLIENTS && event.xcrossing.window == desktop) {
                    focus_desktop();
                }
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