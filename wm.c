#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PANEL_HEIGHT 32
#define TITLE_HEIGHT 24
#define BORDER 5
#define CORNER 16
#define BUTTON_WIDTH 24
#define MAX_CLIENTS 64
#define WORKSPACES 4
#define MIN_WIDTH 120
#define MIN_HEIGHT 60
#define MARGIN 12
#define SNAP_EDGE 6
#define DOUBLE_CLICK_MS 400
#define KEY_COUNT 19

enum { KIND_NORMAL, KIND_PANEL, KIND_DESKTOP, KIND_NOTIFY };
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
enum { LAYER_BELOW, LAYER_NORMAL, LAYER_ABOVE };
enum { PLACE_FREE, PLACE_MAX, PLACE_LEFT, PLACE_RIGHT };
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8 };
enum { HIT_NONE, HIT_TITLE, HIT_CLOSE, HIT_MAXIMIZE, HIT_MINIMIZE, HIT_EDGE };

typedef struct {
    int used;
    Window client;
    Window frame;
    int workspace;
    int sticky;
    int layer;
    int minimized;
    int by_desktop;
    int place;
    int fixed;
    int cursor_edges;
    int x;
    int y;
    int width;
    int height;
    int saved_x;
    int saved_y;
    int saved_width;
    int saved_height;
    int min_width;
    int min_height;
} Client;

static Display *display;
static Window root;
static GC gc;
static XFontStruct *font;
static Cursor cursors[16];
static Cursor cursor_move;
static unsigned long color_active;
static unsigned long color_inactive;
static unsigned long color_active_text;
static unsigned long color_inactive_text;
static unsigned long color_outline;
static Client clients[MAX_CLIENTS];
static int order[MAX_CLIENTS];
static int order_count = 0;
static int active = MAX_CLIENTS;
static int current = 0;
static int screen_number;
static int screen_width;
static int screen_height;
static int body_height;
static int running = 1;
static int showing_desktop = 0;
static int other_wm = 0;
static Window panel;
static Window desktop;
static Window notice;
static int drag_mode = DRAG_NONE;
static int drag_index;
static int drag_edges;
static int drag_moved;
static int drag_pointer_x;
static int drag_pointer_y;
static int drag_x;
static int drag_y;
static int drag_width;
static int drag_height;
static Window last_click_frame = 0;
static Time last_click_time = 0;
static int cycling = 0;
static int cycle_list[MAX_CLIENTS];
static int cycle_count = 0;
static int cycle_pos = 0;
static Atom atom_client_list;
static Atom atom_active_window;
static Atom atom_current_desktop;
static Atom atom_number_of_desktops;
static Atom atom_close_window;
static Atom atom_name;
static Atom atom_utf8;
static Atom atom_protocols;
static Atom atom_delete;
static Atom atom_net_state;
static Atom atom_state_hidden;
static Atom atom_state_vert;
static Atom atom_state_horz;
static Atom atom_state_above;
static Atom atom_state_below;
static Atom atom_state_sticky;
static Atom atom_wm_desktop;
static Atom atom_frame_extents;
static Atom atom_wm_state;
static Atom atom_change_state;
static Atom atom_showing_desktop;
static Atom atom_supported;
static Atom atom_check;
static Atom atom_workarea;

static void switch_workspace_to(int n);

static int subtract(int a, int b) {
    return a + (~b + 1);
}

static int larger(int a, int b) {
    return a > b ? a : b;
}

static int smaller(int a, int b) {
    return a < b ? a : b;
}

static int ignore_error(Display *d, XErrorEvent *e) {
    return 0;
}

static int startup_error(Display *d, XErrorEvent *e) {
    other_wm = 1;
    return 0;
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

static unsigned long pixel(const char *name, unsigned long fallback) {
    XColor screen_color;
    XColor exact_color;

    if (XAllocNamedColor(display, DefaultColormap(display, screen_number), name,
                         &screen_color, &exact_color)) {
        return screen_color.pixel;
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

static int inner_width(int i) {
    return subtract(clients[i].width, 2 * BORDER);
}

static int inner_height(int i) {
    return subtract(clients[i].height, TITLE_HEIGHT + BORDER);
}

static int visible_on(int i, int n) {
    return clients[i].sticky || clients[i].workspace == n;
}

static int min_outer_width(int i) {
    return larger(MIN_WIDTH, clients[i].min_width) + 2 * BORDER;
}

static int min_outer_height(int i) {
    return larger(MIN_HEIGHT, clients[i].min_height) + TITLE_HEIGHT + BORDER;
}

static void order_remove(int i) {
    int k;
    int n = 0;

    for (k = 0; k < order_count; k++) {
        if (order[k] != i) {
            order[n] = order[k];
            n++;
        }
    }
    order_count = n;
}

static void order_front(int i) {
    int k;

    order_remove(i);
    for (k = order_count; k > 0; k = subtract(k, 1)) {
        order[k] = order[subtract(k, 1)];
    }
    order[0] = i;
    order_count++;
}

static void order_back(int i) {
    order_remove(i);
    order[order_count] = i;
    order_count++;
}

static int top_visible(void) {
    int k;
    int i;

    for (k = 0; k < order_count; k++) {
        i = order[k];
        if (visible_on(i, current) && !clients[i].minimized) {
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
    unsigned long showing = (unsigned long)showing_desktop;
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
    XChangeProperty(display, root, atom_showing_desktop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&showing, 1);
    XFlush(display);
}

static void setup_ewmh(void) {
    Atom supported[17];
    unsigned long area[4 * WORKSPACES];
    Window check;
    int n = 0;
    int i;

    supported[n] = atom_client_list; n++;
    supported[n] = atom_active_window; n++;
    supported[n] = atom_current_desktop; n++;
    supported[n] = atom_number_of_desktops; n++;
    supported[n] = atom_close_window; n++;
    supported[n] = atom_net_state; n++;
    supported[n] = atom_state_hidden; n++;
    supported[n] = atom_state_vert; n++;
    supported[n] = atom_state_horz; n++;
    supported[n] = atom_state_above; n++;
    supported[n] = atom_state_below; n++;
    supported[n] = atom_state_sticky; n++;
    supported[n] = atom_wm_desktop; n++;
    supported[n] = atom_frame_extents; n++;
    supported[n] = atom_workarea; n++;
    supported[n] = atom_showing_desktop; n++;
    supported[n] = atom_check; n++;
    XChangeProperty(display, root, atom_supported, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported, n);

    for (i = 0; i < WORKSPACES; i++) {
        area[4 * i] = 0;
        area[4 * i + 1] = PANEL_HEIGHT;
        area[4 * i + 2] = (unsigned long)screen_width;
        area[4 * i + 3] = (unsigned long)body_height;
    }
    XChangeProperty(display, root, atom_workarea, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)area, 4 * WORKSPACES);

    check = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(display, check, atom_check, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check, 1);
    XChangeProperty(display, check, atom_name, atom_utf8, 8, PropModeReplace,
                    (unsigned char *)"DE", 2);
    XChangeProperty(display, root, atom_check, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check, 1);
}

static void set_wm_state(int i, unsigned long state) {
    unsigned long data[2];

    data[0] = state;
    data[1] = None;
    XChangeProperty(display, clients[i].client, atom_wm_state, atom_wm_state, 32,
                    PropModeReplace, (unsigned char *)data, 2);
}

static void update_state(int i) {
    Atom list[8];
    unsigned long value;
    int n = 0;

    if (clients[i].minimized) {
        list[n] = atom_state_hidden; n++;
    }
    if (clients[i].place == PLACE_MAX) {
        list[n] = atom_state_vert; n++;
        list[n] = atom_state_horz; n++;
    }
    if (clients[i].layer == LAYER_ABOVE) {
        list[n] = atom_state_above; n++;
    }
    if (clients[i].layer == LAYER_BELOW) {
        list[n] = atom_state_below; n++;
    }
    if (clients[i].sticky) {
        list[n] = atom_state_sticky; n++;
    }
    XChangeProperty(display, clients[i].client, atom_net_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)list, n);
    value = clients[i].sticky ? 0xFFFFFFFFul : (unsigned long)clients[i].workspace;
    XChangeProperty(display, clients[i].client, atom_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&value, 1);
}

static void draw_frame(int i) {
    char title[256];
    char shown[256];
    int is_active = (i == active);
    int width = clients[i].width;
    int button_x = subtract(width, BORDER + 3 * BUTTON_WIDTH);
    int middle = TITLE_HEIGHT / 2;
    int ascent = font != NULL ? (*font).ascent : 10;
    int descent = font != NULL ? (*font).descent : 3;
    int char_width = font != NULL ? larger((*font).max_bounds.width, 1) : 6;
    int room = subtract(button_x, BORDER + 10);
    int length;
    int fit;
    int n;
    int base;
    int cx;
    unsigned long background = is_active ? color_active : color_inactive;
    unsigned long foreground = is_active ? color_active_text : color_inactive_text;

    get_title(clients[i].client, title, sizeof(title));
    length = (int)strlen(title);
    for (n = 0; n < length; n++) {
        if ((unsigned char)title[n] > 126) {
            title[n] = '?';
        }
    }
    if (room < 0) {
        room = 0;
    }
    fit = room / char_width;
    if (length > fit) {
        if (fit > 3) {
            memcpy(shown, title, (size_t)subtract(fit, 3));
            memcpy(shown + subtract(fit, 3), "...", 3);
        } else {
            memcpy(shown, title, (size_t)fit);
        }
        length = fit;
    } else {
        memcpy(shown, title, (size_t)length);
    }

    XSetWindowBackground(display, clients[i].frame, background);
    XClearWindow(display, clients[i].frame);
    XSetForeground(display, gc, color_outline);
    XDrawRectangle(display, clients[i].frame, gc, 0, 0, (unsigned int)subtract(width, 1),
                   (unsigned int)subtract(clients[i].height, 1));
    XSetForeground(display, gc, foreground);
    base = middle + subtract(ascent, descent) / 2;
    XDrawString(display, clients[i].frame, gc, BORDER + 6, base, shown, length);

    cx = button_x + BUTTON_WIDTH / 2;
    XDrawLine(display, clients[i].frame, gc, subtract(cx, 5), middle + 4, cx + 5, middle + 4);
    XDrawLine(display, clients[i].frame, gc, subtract(cx, 5), middle + 3, cx + 5, middle + 3);
    cx += BUTTON_WIDTH;
    XDrawRectangle(display, clients[i].frame, gc, subtract(cx, 5), subtract(middle, 5), 10, 10);
    XDrawLine(display, clients[i].frame, gc, subtract(cx, 5), subtract(middle, 4), cx + 5,
              subtract(middle, 4));
    cx += BUTTON_WIDTH;
    XDrawLine(display, clients[i].frame, gc, subtract(cx, 5), subtract(middle, 5), cx + 5,
              middle + 5);
    XDrawLine(display, clients[i].frame, gc, subtract(cx, 5), middle + 5, cx + 5,
              subtract(middle, 5));
}

static void send_configure(int i) {
    XConfigureEvent configure;

    memset(&configure, 0, sizeof(configure));
    configure.type = ConfigureNotify;
    configure.display = display;
    configure.event = clients[i].client;
    configure.window = clients[i].client;
    configure.x = clients[i].x + BORDER;
    configure.y = clients[i].y + TITLE_HEIGHT;
    configure.width = inner_width(i);
    configure.height = inner_height(i);
    configure.border_width = 0;
    configure.above = None;
    configure.override_redirect = False;
    XSendEvent(display, clients[i].client, False, StructureNotifyMask, (XEvent *)&configure);
}

static void apply_geometry(int i) {
    XMoveResizeWindow(display, clients[i].frame, clients[i].x, clients[i].y,
                      (unsigned int)clients[i].width, (unsigned int)clients[i].height);
    XResizeWindow(display, clients[i].client, (unsigned int)inner_width(i),
                  (unsigned int)inner_height(i));
    send_configure(i);
}

static void place_below(Window w, Window *anchor) {
    XWindowChanges changes;

    if (w == 0) {
        return;
    }
    if (*anchor == 0) {
        XRaiseWindow(display, w);
    } else {
        changes.sibling = *anchor;
        changes.stack_mode = Below;
        XConfigureWindow(display, w, CWSibling | CWStackMode, &changes);
    }
    *anchor = w;
}

static void restack(void) {
    Window anchor = panel;
    int step;
    int layer;
    int k;
    int i;

    for (step = 0; step < 3; step++) {
        layer = subtract(LAYER_ABOVE, step);
        for (k = 0; k < order_count; k++) {
            i = order[k];
            if (clients[i].layer == layer) {
                place_below(clients[i].frame, &anchor);
            }
        }
    }
    place_below(desktop, &anchor);
}

static void set_focus(int i) {
    int old = active;

    active = i;
    if (i == MAX_CLIENTS) {
        XSetInputFocus(display, root, RevertToPointerRoot, CurrentTime);
    } else {
        XSetInputFocus(display, clients[i].client, RevertToPointerRoot, CurrentTime);
    }
    if (old != MAX_CLIENTS && old != i && clients[old].used) {
        draw_frame(old);
    }
    if (i != MAX_CLIENTS) {
        draw_frame(i);
    }
    publish();
}

static void activate(int i) {
    int k;

    if (i == MAX_CLIENTS) {
        set_focus(MAX_CLIENTS);
        return;
    }
    showing_desktop = 0;
    for (k = 0; k < MAX_CLIENTS; k++) {
        clients[k].by_desktop = 0;
    }
    order_front(i);
    restack();
    set_focus(i);
}

static void set_minimized(int i, int flag) {
    clients[i].minimized = flag;
    if (flag) {
        XUnmapWindow(display, clients[i].frame);
        set_wm_state(i, IconicState);
    } else {
        if (visible_on(i, current)) {
            XMapWindow(display, clients[i].frame);
        }
        set_wm_state(i, NormalState);
    }
    update_state(i);
}

static void minimize_client(int i) {
    if (i == MAX_CLIENTS || clients[i].minimized) {
        return;
    }
    set_minimized(i, 1);
    if (active == i) {
        activate(top_visible());
    } else {
        publish();
    }
}

static void present(int i) {
    if (!clients[i].sticky && clients[i].workspace != current) {
        switch_workspace_to(clients[i].workspace);
    }
    if (clients[i].minimized) {
        set_minimized(i, 0);
    }
    activate(i);
}

static void lower_client(int i) {
    order_back(i);
    restack();
    if (active == i) {
        activate(top_visible());
    }
}

static void show_desktop(int hide) {
    int i;
    int k;

    if (hide) {
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].used && visible_on(i, current) && !clients[i].minimized) {
                set_minimized(i, 1);
                clients[i].by_desktop = 1;
            }
        }
        showing_desktop = 1;
        set_focus(MAX_CLIENTS);
        return;
    }
    for (k = 0; k < order_count; k++) {
        i = order[k];
        if (clients[i].by_desktop && visible_on(i, current)) {
            set_minimized(i, 0);
        }
    }
    activate(top_visible());
}

static void set_layer(int i, int layer) {
    clients[i].layer = layer;
    restack();
    update_state(i);
}

static void set_sticky(int i, int on) {
    clients[i].sticky = on;
    if (on) {
        if (!clients[i].minimized) {
            XMapWindow(display, clients[i].frame);
        }
    } else {
        clients[i].workspace = current;
    }
    update_state(i);
}

static void place_client(int i, int place) {
    int x = 0;
    int y = PANEL_HEIGHT;
    int width = screen_width;
    int half = screen_width / 2;

    if (clients[i].fixed) {
        return;
    }
    if (place == PLACE_FREE) {
        if (clients[i].place == PLACE_FREE) {
            return;
        }
        clients[i].x = clients[i].saved_x;
        clients[i].y = clients[i].saved_y;
        clients[i].width = clients[i].saved_width;
        clients[i].height = clients[i].saved_height;
    } else {
        if (clients[i].place == PLACE_FREE) {
            clients[i].saved_x = clients[i].x;
            clients[i].saved_y = clients[i].y;
            clients[i].saved_width = clients[i].width;
            clients[i].saved_height = clients[i].height;
        }
        if (place == PLACE_LEFT) {
            width = half;
        } else if (place == PLACE_RIGHT) {
            x = half;
            width = subtract(screen_width, half);
        }
        clients[i].x = x;
        clients[i].y = y;
        clients[i].width = width;
        clients[i].height = body_height;
    }
    clients[i].place = place;
    apply_geometry(i);
    update_state(i);
}

static void toggle_maximize(int i) {
    if (i == MAX_CLIENTS) {
        return;
    }
    place_client(i, clients[i].place == PLACE_MAX ? PLACE_FREE : PLACE_MAX);
}

static void switch_workspace_to(int n) {
    int i;

    if (n < 0 || n >= WORKSPACES) {
        return;
    }
    current = n;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            continue;
        }
        if (visible_on(i, current) && !clients[i].minimized) {
            XMapWindow(display, clients[i].frame);
        } else {
            XUnmapWindow(display, clients[i].frame);
        }
    }
    activate(top_visible());
}

static void move_to_workspace(int i, int n) {
    if (i == MAX_CLIENTS || n < 0 || n >= WORKSPACES) {
        return;
    }
    clients[i].sticky = 0;
    clients[i].workspace = n;
    if (n != current) {
        XUnmapWindow(display, clients[i].frame);
        if (active == i) {
            activate(top_visible());
        }
    } else if (!clients[i].minimized) {
        XMapWindow(display, clients[i].frame);
    }
    update_state(i);
    publish();
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
    XSizeHints hints;
    long supplied;
    Window parent = None;
    unsigned long extents[4];
    static int cascade = 0;
    int i;
    int width;
    int height;
    int offset;

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

    memset(&clients[i], 0, sizeof(Client));
    if (XGetWMNormalHints(display, w, &hints, &supplied)) {
        if (hints.flags & PMinSize) {
            clients[i].min_width = hints.min_width;
            clients[i].min_height = hints.min_height;
        }
        if ((hints.flags & PMinSize) && (hints.flags & PMaxSize) &&
            hints.min_width == hints.max_width && hints.min_height == hints.max_height) {
            clients[i].fixed = 1;
        }
    }
    width = larger(width, larger(MIN_WIDTH, clients[i].min_width));
    height = larger(height, larger(MIN_HEIGHT, clients[i].min_height));
    width = smaller(width, subtract(screen_width, 2 * BORDER));
    height = smaller(height, subtract(body_height, TITLE_HEIGHT + BORDER));

    clients[i].used = 1;
    clients[i].client = w;
    clients[i].workspace = current;
    clients[i].layer = LAYER_NORMAL;
    clients[i].place = PLACE_FREE;
    clients[i].width = width + 2 * BORDER;
    clients[i].height = height + TITLE_HEIGHT + BORDER;

    if (XGetTransientForHint(display, w, &parent) && parent != None) {
        clients[i].x = subtract(screen_width, clients[i].width) / 2;
        clients[i].y = PANEL_HEIGHT + subtract(body_height, clients[i].height) / 2;
    } else {
        offset = (cascade % 8) * 24;
        cascade++;
        clients[i].x = 40 + offset;
        clients[i].y = PANEL_HEIGHT + 20 + offset;
        if (clients[i].x + clients[i].width > screen_width) {
            clients[i].x = larger(0, subtract(screen_width, clients[i].width));
        }
        if (clients[i].y + clients[i].height > screen_height) {
            clients[i].y = larger(PANEL_HEIGHT, subtract(screen_height, clients[i].height));
        }
    }
    if (clients[i].x < 0) {
        clients[i].x = 0;
    }

    clients[i].frame = XCreateSimpleWindow(display, root, clients[i].x, clients[i].y,
                                           (unsigned int)clients[i].width,
                                           (unsigned int)clients[i].height, 0,
                                           color_outline, color_inactive);
    clients[i].cursor_edges = 0;
    XDefineCursor(display, clients[i].frame, cursors[0]);
    XSelectInput(display, clients[i].frame,
                 SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask |
                 ExposureMask | PointerMotionMask);
    XSelectInput(display, w, PropertyChangeMask);
    XAddToSaveSet(display, w);
    XResizeWindow(display, w, (unsigned int)width, (unsigned int)height);
    XReparentWindow(display, w, clients[i].frame, BORDER, TITLE_HEIGHT);
    XGrabButton(display, AnyButton, AnyModifier, w, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);

    extents[0] = BORDER;
    extents[1] = BORDER;
    extents[2] = TITLE_HEIGHT;
    extents[3] = BORDER;
    XChangeProperty(display, w, atom_frame_extents, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)extents, 4);

    XMapWindow(display, w);
    XMapWindow(display, clients[i].frame);
    set_wm_state(i, NormalState);
    update_state(i);
    send_configure(i);
    activate(i);
}

static void remove_client(int i, int alive) {
    if (alive) {
        XReparentWindow(display, clients[i].client, root, clients[i].x, clients[i].y);
        XRemoveFromSaveSet(display, clients[i].client);
        XDeleteProperty(display, clients[i].client, atom_net_state);
        XDeleteProperty(display, clients[i].client, atom_wm_state);
    }
    XDestroyWindow(display, clients[i].frame);
    clients[i].used = 0;
    order_remove(i);
    if (drag_mode != DRAG_NONE && drag_index == i) {
        drag_mode = DRAG_NONE;
        XUngrabPointer(display, CurrentTime);
    }
    if (active == i) {
        active = MAX_CLIENTS;
        activate(top_visible());
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
        XMoveResizeWindow(display, w, 0, 0, (unsigned int)screen_width, PANEL_HEIGHT);
        XMapWindow(display, w);
        XRaiseWindow(display, w);
        return;
    }
    if (kind == KIND_DESKTOP) {
        desktop = w;
        XMoveResizeWindow(display, w, 0, PANEL_HEIGHT, (unsigned int)screen_width,
                          (unsigned int)body_height);
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
        if (clients[i].place == PLACE_FREE) {
            if (request.value_mask & CWWidth) {
                clients[i].width = request.width + 2 * BORDER;
            }
            if (request.value_mask & CWHeight) {
                clients[i].height = request.height + TITLE_HEIGHT + BORDER;
            }
            apply_geometry(i);
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
                          PANEL_HEIGHT + MARGIN, (unsigned int)request.width,
                          (unsigned int)request.height);
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

static void preview(int i) {
    XWindowChanges changes;

    if (panel != 0) {
        changes.sibling = panel;
        changes.stack_mode = Below;
        XConfigureWindow(display, clients[i].frame, CWSibling | CWStackMode, &changes);
    } else {
        XRaiseWindow(display, clients[i].frame);
    }
    set_focus(i);
}

static void cycle_step(int backwards) {
    int step = backwards ? subtract(cycle_count, 1) : 1;
    int tries;
    int i;

    for (tries = 0; tries < cycle_count; tries++) {
        cycle_pos = (cycle_pos + step) % cycle_count;
        i = cycle_list[cycle_pos];
        if (clients[i].used && !clients[i].minimized && visible_on(i, current)) {
            preview(i);
            return;
        }
    }
}

static void end_cycle(void) {
    if (!cycling) {
        return;
    }
    cycling = 0;
    XUngrabKeyboard(display, CurrentTime);
    if (active != MAX_CLIENTS && clients[active].used) {
        activate(active);
    }
}

static void begin_cycle(int backwards) {
    Window ignored_root;
    Window ignored_child;
    int ignored_x;
    int ignored_y;
    unsigned int mask = 0;
    int k;
    int i;

    cycle_count = 0;
    for (k = 0; k < order_count; k++) {
        i = order[k];
        if (visible_on(i, current) && !clients[i].minimized) {
            cycle_list[cycle_count] = i;
            cycle_count++;
        }
    }
    if (cycle_count == 0) {
        for (k = 0; k < order_count; k++) {
            i = order[k];
            if (visible_on(i, current) && clients[i].minimized) {
                present(i);
                return;
            }
        }
        return;
    }
    if (cycle_count < 2) {
        return;
    }
    if (XGrabKeyboard(display, root, False, GrabModeAsync, GrabModeAsync,
                      CurrentTime) != GrabSuccess) {
        activate(cycle_list[1]);
        return;
    }
    cycling = 1;
    cycle_pos = 0;
    cycle_step(backwards);
    XQueryPointer(display, root, &ignored_root, &ignored_child, &ignored_x, &ignored_y,
                  &ignored_x, &ignored_y, &mask);
    if ((mask & Mod1Mask) == 0) {
        end_cycle();
    }
}

static void on_key(XKeyEvent key) {
    KeySym symbol = XLookupKeysym(&key, 0);
    int shift = (key.state & ShiftMask) != 0;
    int control = (key.state & ControlMask) != 0;
    int number;

    if (key.type == KeyRelease) {
        if (cycling && (symbol == XK_Alt_L || symbol == XK_Alt_R ||
                        symbol == XK_Meta_L || symbol == XK_Meta_R)) {
            end_cycle();
        }
        return;
    }
    if ((key.state & Mod1Mask) == 0) {
        return;
    }
    if (symbol == XK_Tab) {
        if (cycling) {
            cycle_step(shift);
        } else {
            begin_cycle(shift);
        }
        return;
    }
    if (control && (symbol == XK_Left || symbol == XK_Right)) {
        number = symbol == XK_Right ? (current + 1) % WORKSPACES
                                    : (current + WORKSPACES + subtract(0, 1)) % WORKSPACES;
        switch_workspace_to(number);
        return;
    }
    if (symbol == XK_Return) {
        spawn("./terminal");
    } else if (symbol == XK_f) {
        spawn("./files");
    } else if (symbol == XK_s) {
        spawn("./settings");
    } else if (symbol == XK_q || symbol == XK_F4) {
        close_client(active);
    } else if (symbol == XK_e) {
        running = 0;
    } else if (symbol == XK_Left && active != MAX_CLIENTS) {
        place_client(active, clients[active].place == PLACE_LEFT ? PLACE_FREE : PLACE_LEFT);
    } else if (symbol == XK_Right && active != MAX_CLIENTS) {
        place_client(active, clients[active].place == PLACE_RIGHT ? PLACE_FREE : PLACE_RIGHT);
    } else if (symbol == XK_Up) {
        toggle_maximize(active);
    } else if (symbol == XK_Down) {
        minimize_client(active);
    } else if (symbol == XK_d) {
        show_desktop(!showing_desktop);
    } else if (symbol == XK_t && active != MAX_CLIENTS) {
        set_layer(active, clients[active].layer == LAYER_ABOVE ? LAYER_NORMAL : LAYER_ABOVE);
    } else if (symbol == XK_b && active != MAX_CLIENTS) {
        set_layer(active, clients[active].layer == LAYER_BELOW ? LAYER_NORMAL : LAYER_BELOW);
    } else if (symbol == XK_y && active != MAX_CLIENTS) {
        set_sticky(active, !clients[active].sticky);
    } else if (symbol >= XK_1 && symbol < XK_1 + WORKSPACES) {
        number = subtract((int)symbol, (int)XK_1);
        if (!shift) {
            switch_workspace_to(number);
        } else {
            move_to_workspace(active, number);
        }
    }
}

static int hit_test(int i, int x, int y, int *edges) {
    int width = clients[i].width;
    int height = clients[i].height;
    int button_x = subtract(width, BORDER + 3 * BUTTON_WIDTH);
    int found = 0;

    *edges = 0;
    if (!clients[i].fixed && clients[i].place == PLACE_FREE) {
        if (x < BORDER) {
            found |= EDGE_LEFT;
        }
        if (x >= subtract(width, BORDER)) {
            found |= EDGE_RIGHT;
        }
        if (y < BORDER) {
            found |= EDGE_TOP;
        }
        if (y >= subtract(height, BORDER)) {
            found |= EDGE_BOTTOM;
        }
        if (found != 0) {
            if ((found & (EDGE_LEFT | EDGE_RIGHT)) && !(found & (EDGE_TOP | EDGE_BOTTOM))) {
                if (y < CORNER) {
                    found |= EDGE_TOP;
                } else if (y >= subtract(height, CORNER)) {
                    found |= EDGE_BOTTOM;
                }
            }
            if ((found & (EDGE_TOP | EDGE_BOTTOM)) && !(found & (EDGE_LEFT | EDGE_RIGHT))) {
                if (x < CORNER) {
                    found |= EDGE_LEFT;
                } else if (x >= subtract(width, CORNER)) {
                    found |= EDGE_RIGHT;
                }
            }
            *edges = found;
            return HIT_EDGE;
        }
    }
    if (y < TITLE_HEIGHT) {
        if (x >= button_x + 2 * BUTTON_WIDTH && x < button_x + 3 * BUTTON_WIDTH) {
            return HIT_CLOSE;
        }
        if (x >= button_x + BUTTON_WIDTH && x < button_x + 2 * BUTTON_WIDTH) {
            return HIT_MAXIMIZE;
        }
        if (x >= button_x && x < button_x + BUTTON_WIDTH) {
            return HIT_MINIMIZE;
        }
        return HIT_TITLE;
    }
    return HIT_NONE;
}

static void start_drag(int i, int mode, int edges, XButtonEvent press) {
    drag_mode = mode;
    drag_index = i;
    drag_edges = edges;
    drag_moved = 0;
    drag_pointer_x = press.x_root;
    drag_pointer_y = press.y_root;
    drag_x = clients[i].x;
    drag_y = clients[i].y;
    drag_width = clients[i].width;
    drag_height = clients[i].height;
    XGrabPointer(display, root, False, ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None,
                 mode == DRAG_RESIZE ? cursors[edges] : cursor_move, CurrentTime);
}

static void start_move(int i, XButtonEvent press) {
    int width;
    int inside;

    if (clients[i].place != PLACE_FREE) {
        width = clients[i].saved_width;
        inside = (width * press.x_root) / screen_width;
        clients[i].x = subtract(press.x_root, inside);
        clients[i].y = larger(PANEL_HEIGHT, subtract(press.y_root, TITLE_HEIGHT / 2));
        clients[i].width = width;
        clients[i].height = clients[i].saved_height;
        clients[i].place = PLACE_FREE;
        apply_geometry(i);
        update_state(i);
    }
    start_drag(i, DRAG_MOVE, 0, press);
}

static void finish_drag(XButtonEvent release) {
    int i = drag_index;
    int moved = drag_moved;
    int mode = drag_mode;

    drag_mode = DRAG_NONE;
    XUngrabPointer(display, CurrentTime);
    if (mode != DRAG_MOVE || !moved || !clients[i].used || clients[i].fixed) {
        return;
    }
    if (release.y_root <= SNAP_EDGE) {
        place_client(i, PLACE_MAX);
    } else if (release.x_root <= SNAP_EDGE) {
        place_client(i, PLACE_LEFT);
    } else if (release.x_root >= subtract(screen_width, 1 + SNAP_EDGE)) {
        place_client(i, PLACE_RIGHT);
    }
}

static void on_button_press(XButtonEvent press) {
    int alt = (press.state & Mod1Mask) != 0;
    Window target = press.window;
    Window last_frame;
    Time last_time;
    int edges = 0;
    int hit;
    int i;
    int centre_x;
    int centre_y;

    i = find_by_client(press.window);
    if (i != MAX_CLIENTS) {
        activate(i);
        XAllowEvents(display, ReplayPointer, CurrentTime);
        return;
    }
    if (alt) {
        target = press.subwindow;
    }
    i = find_by_frame(target);
    if (i == MAX_CLIENTS) {
        return;
    }
    if (alt) {
        activate(i);
        if (press.button == Button1) {
            start_move(i, press);
        } else if (press.button == Button3 && !clients[i].fixed &&
                   clients[i].place == PLACE_FREE) {
            centre_x = clients[i].x + clients[i].width / 2;
            centre_y = clients[i].y + clients[i].height / 2;
            edges = (press.x_root < centre_x ? EDGE_LEFT : EDGE_RIGHT) |
                    (press.y_root < centre_y ? EDGE_TOP : EDGE_BOTTOM);
            start_drag(i, DRAG_RESIZE, edges, press);
        }
        return;
    }
    hit = hit_test(i, press.x, press.y, &edges);
    if (press.button == Button2 && hit == HIT_TITLE) {
        lower_client(i);
        return;
    }
    activate(i);
    if (press.button != Button1) {
        return;
    }
    if (hit == HIT_CLOSE) {
        close_client(i);
    } else if (hit == HIT_MINIMIZE) {
        minimize_client(i);
    } else if (hit == HIT_MAXIMIZE) {
        toggle_maximize(i);
    } else if (hit == HIT_EDGE) {
        start_drag(i, DRAG_RESIZE, edges, press);
    } else if (hit == HIT_TITLE) {
        last_frame = last_click_frame;
        last_time = last_click_time;
        last_click_frame = clients[i].frame;
        last_click_time = press.time;
        if (last_frame == clients[i].frame &&
            subtract((int)press.time, (int)last_time) < DOUBLE_CLICK_MS) {
            last_click_frame = 0;
            toggle_maximize(i);
        } else {
            start_move(i, press);
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
    int edges;
    int hit;
    int i;
    int right;
    int bottom;

    if (drag_mode == DRAG_NONE) {
        i = find_by_frame(motion.window);
        if (i == MAX_CLIENTS) {
            return;
        }
        hit = hit_test(i, motion.x, motion.y, &edges);
        if (hit != HIT_EDGE) {
            edges = 0;
        }
        if (edges != clients[i].cursor_edges) {
            clients[i].cursor_edges = edges;
            XDefineCursor(display, clients[i].frame, cursors[edges]);
        }
        return;
    }
    if (!clients[drag_index].used) {
        return;
    }
    i = drag_index;
    dx = subtract(motion.x_root, drag_pointer_x);
    dy = subtract(motion.y_root, drag_pointer_y);
    if (dx != 0 || dy != 0) {
        drag_moved = 1;
    }
    if (drag_mode == DRAG_MOVE) {
        x = drag_x + dx;
        y = drag_y + dy;
        if (y < PANEL_HEIGHT) {
            y = PANEL_HEIGHT;
        }
        if (y > subtract(screen_height, TITLE_HEIGHT)) {
            y = subtract(screen_height, TITLE_HEIGHT);
        }
        clients[i].x = x;
        clients[i].y = y;
        XMoveWindow(display, clients[i].frame, x, y);
        send_configure(i);
        return;
    }
    x = drag_x;
    y = drag_y;
    width = drag_width;
    height = drag_height;
    right = drag_x + drag_width;
    bottom = drag_y + drag_height;
    if (drag_edges & EDGE_RIGHT) {
        width = drag_width + dx;
    }
    if (drag_edges & EDGE_LEFT) {
        width = subtract(drag_width, dx);
    }
    if (drag_edges & EDGE_BOTTOM) {
        height = drag_height + dy;
    }
    if (drag_edges & EDGE_TOP) {
        height = subtract(drag_height, dy);
    }
    width = larger(width, min_outer_width(i));
    height = larger(height, min_outer_height(i));
    if (drag_edges & EDGE_LEFT) {
        x = subtract(right, width);
    }
    if (drag_edges & EDGE_TOP) {
        y = subtract(bottom, height);
        if (y < PANEL_HEIGHT) {
            y = PANEL_HEIGHT;
            height = subtract(bottom, y);
        }
    }
    clients[i].x = x;
    clients[i].y = y;
    clients[i].width = width;
    clients[i].height = height;
    apply_geometry(i);
}

static void apply_state(int i, Atom state, int action) {
    int on;

    if (state == atom_state_vert || state == atom_state_horz) {
        on = action == 2 ? clients[i].place != PLACE_MAX : action;
        place_client(i, on ? PLACE_MAX : PLACE_FREE);
    } else if (state == atom_state_above) {
        on = action == 2 ? clients[i].layer != LAYER_ABOVE : action;
        set_layer(i, on ? LAYER_ABOVE : LAYER_NORMAL);
    } else if (state == atom_state_below) {
        on = action == 2 ? clients[i].layer != LAYER_BELOW : action;
        set_layer(i, on ? LAYER_BELOW : LAYER_NORMAL);
    } else if (state == atom_state_sticky) {
        on = action == 2 ? !clients[i].sticky : action;
        set_sticky(i, on);
    } else if (state == atom_state_hidden) {
        on = action == 2 ? !clients[i].minimized : action;
        if (on) {
            minimize_client(i);
        } else if (clients[i].minimized) {
            present(i);
        }
    }
}

static void on_client_message(XClientMessageEvent message) {
    int i;
    Atom second;

    if (message.message_type == atom_active_window) {
        i = find_by_client(message.window);
        if (i != MAX_CLIENTS) {
            present(i);
        }
    } else if (message.message_type == atom_current_desktop) {
        switch_workspace_to((int)message.data.l[0]);
    } else if (message.message_type == atom_close_window) {
        close_client(find_by_client(message.window));
    } else if (message.message_type == atom_change_state) {
        i = find_by_client(message.window);
        if (i != MAX_CLIENTS && message.data.l[0] == IconicState) {
            minimize_client(i);
        }
    } else if (message.message_type == atom_showing_desktop) {
        show_desktop(message.data.l[0] != 0);
    } else if (message.message_type == atom_wm_desktop) {
        i = find_by_client(message.window);
        if (i == MAX_CLIENTS) {
            return;
        }
        if ((unsigned long)message.data.l[0] == 0xFFFFFFFFul) {
            set_sticky(i, 1);
        } else {
            move_to_workspace(i, (int)message.data.l[0]);
        }
    } else if (message.message_type == atom_net_state) {
        i = find_by_client(message.window);
        if (i == MAX_CLIENTS) {
            return;
        }
        apply_state(i, (Atom)message.data.l[1], (int)message.data.l[0]);
        second = (Atom)message.data.l[2];
        if (second != 0 &&
            !((message.data.l[1] == (long)atom_state_vert || message.data.l[1] == (long)atom_state_horz) &&
              (second == atom_state_vert || second == atom_state_horz))) {
            apply_state(i, second, (int)message.data.l[0]);
        }
    }
}

static void bind_keys(void) {
    KeySym symbols[KEY_COUNT] = {
        XK_Return, XK_f, XK_s, XK_q, XK_F4, XK_Tab, XK_e, XK_1, XK_2, XK_3, XK_4,
        XK_Left, XK_Right, XK_Up, XK_Down, XK_d, XK_t, XK_b, XK_y
    };
    unsigned int variants[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    unsigned int shifts[2] = {0, ShiftMask};
    unsigned int controls[2] = {0, ControlMask};
    unsigned int buttons[2] = {Button1, Button3};
    int i;
    int j;
    int k;
    int c;

    for (i = 0; i < KEY_COUNT; i++) {
        KeyCode code = XKeysymToKeycode(display, symbols[i]);
        if (code == 0) {
            continue;
        }
        for (j = 0; j < 4; j++) {
            for (k = 0; k < 2; k++) {
                for (c = 0; c < 2; c++) {
                    XGrabKey(display, code, Mod1Mask | variants[j] | shifts[k] | controls[c],
                             root, True, GrabModeAsync, GrabModeAsync);
                }
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

static void intern_atoms(void) {
    atom_client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    atom_active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    atom_current_desktop = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
    atom_number_of_desktops = XInternAtom(display, "_NET_NUMBER_OF_DESKTOPS", False);
    atom_close_window = XInternAtom(display, "_NET_CLOSE_WINDOW", False);
    atom_name = XInternAtom(display, "_NET_WM_NAME", False);
    atom_utf8 = XInternAtom(display, "UTF8_STRING", False);
    atom_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    atom_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    atom_net_state = XInternAtom(display, "_NET_WM_STATE", False);
    atom_state_hidden = XInternAtom(display, "_NET_WM_STATE_HIDDEN", False);
    atom_state_vert = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    atom_state_horz = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    atom_state_above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    atom_state_below = XInternAtom(display, "_NET_WM_STATE_BELOW", False);
    atom_state_sticky = XInternAtom(display, "_NET_WM_STATE_STICKY", False);
    atom_wm_desktop = XInternAtom(display, "_NET_WM_DESKTOP", False);
    atom_frame_extents = XInternAtom(display, "_NET_FRAME_EXTENTS", False);
    atom_wm_state = XInternAtom(display, "WM_STATE", False);
    atom_change_state = XInternAtom(display, "WM_CHANGE_STATE", False);
    atom_showing_desktop = XInternAtom(display, "_NET_SHOWING_DESKTOP", False);
    atom_supported = XInternAtom(display, "_NET_SUPPORTED", False);
    atom_check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    atom_workarea = XInternAtom(display, "_NET_WORKAREA", False);
}

static void make_cursors(void) {
    cursors[0] = XCreateFontCursor(display, XC_left_ptr);
    cursors[EDGE_LEFT] = XCreateFontCursor(display, XC_left_side);
    cursors[EDGE_RIGHT] = XCreateFontCursor(display, XC_right_side);
    cursors[EDGE_TOP] = XCreateFontCursor(display, XC_top_side);
    cursors[EDGE_BOTTOM] = XCreateFontCursor(display, XC_bottom_side);
    cursors[EDGE_LEFT | EDGE_TOP] = XCreateFontCursor(display, XC_top_left_corner);
    cursors[EDGE_RIGHT | EDGE_TOP] = XCreateFontCursor(display, XC_top_right_corner);
    cursors[EDGE_LEFT | EDGE_BOTTOM] = XCreateFontCursor(display, XC_bottom_left_corner);
    cursors[EDGE_RIGHT | EDGE_BOTTOM] = XCreateFontCursor(display, XC_bottom_right_corner);
    cursor_move = XCreateFontCursor(display, XC_fleur);
}

int main(void) {
    XEvent event;
    XEvent next;
    char pid[32];
    int i;

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        return 1;
    }
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
        return 1;
    }
    XSetErrorHandler(ignore_error);

    intern_atoms();
    make_cursors();
    font = XLoadQueryFont(display, "fixed");
    gc = XCreateGC(display, root, 0, NULL);
    if (font != NULL) {
        XSetFont(display, gc, (*font).fid);
    }
    color_active = pixel("#3b6ea5", BlackPixel(display, screen_number));
    color_inactive = pixel("#c8ccd2", WhitePixel(display, screen_number));
    color_active_text = pixel("#ffffff", WhitePixel(display, screen_number));
    color_inactive_text = pixel("#40444a", BlackPixel(display, screen_number));
    color_outline = pixel("#1e2a3a", BlackPixel(display, screen_number));
    XSetForeground(display, gc, BlackPixel(display, screen_number));
    XDefineCursor(display, root, cursors[0]);
    XSync(display, False);
    bind_keys();
    setup_ewmh();
    publish();

    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    setenv("DE_SESSION_PID", pid, 1);
    spawn("./desktop");
    spawn("./panel");
    spawn("./notify");

    while (running) {
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
        case KeyRelease:
            on_key(event.xkey);
            break;
        case ButtonPress:
            on_button_press(event.xbutton);
            break;
        case ButtonRelease:
            if (drag_mode != DRAG_NONE) {
                finish_drag(event.xbutton);
            }
            break;
        case MotionNotify:
            while (XCheckTypedEvent(display, MotionNotify, &next)) {
                event = next;
            }
            on_motion(event.xmotion);
            break;
        case Expose:
            i = find_by_frame(event.xexpose.window);
            if (i != MAX_CLIENTS && event.xexpose.count == 0) {
                draw_frame(i);
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
