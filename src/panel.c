// panel.c: minimal GTK4 and EWMH panel for X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/x11/gdkx.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKSPACES   4
#define PANEL_HEIGHT 32
#define PANEL_BOTTOM 0      // 1 docks at the bottom edge, 0 at the top edge
#define MAX_TASKS    64
#define TITLE_LEN    128

typedef struct {
    Window window;
    gboolean hidden;
    gboolean active;
    char title[TITLE_LEN];
} Task;

static Display *xdisplay;
static Window xroot;
static int (*previous_handler)(Display *, XErrorEvent *);

static Atom atom_client_list, atom_active_window, atom_current_desktop;
static Atom atom_name, atom_utf8;
static Atom atom_net_state, atom_state_hidden, atom_state_skip_taskbar;
static Atom atom_change_state, atom_showing_desktop;
static Atom atom_window_type, atom_type_dock, atom_type_desktop;
static Atom atom_strut_partial;

static GtkWidget *clock_label;
static GtkWidget *battery_label;
static GtkWidget *popover;
static GtkWidget *taskbar;
static GtkWidget *workspace_buttons[WORKSPACES];
static gchar *last_signature;
static int last_current = G_MAXINT;

static int subtract(int a, int b) {
    return a + (~b + 1);
}

// X11 helpers

// Windows can vanish between listing and querying, so BadWindow is harmless.
static int handle_error(Display *display, XErrorEvent *event) {
    if ((*event).error_code == BadWindow) {
        return 0;
    }
    return previous_handler != NULL ? previous_handler(display, event) : 0;
}

static void send_message2(Window target, Atom type, long first, long second) {
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = target;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = first;
    event.xclient.data.l[1] = second;
    XSendEvent(xdisplay, xroot, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(xdisplay);
}

static void send_message(Window target, Atom type, long value) {
    send_message2(target, type, value, 0);
}

// Returns an array of longs to release with XFree, or NULL.
static unsigned long *get_longs(Window window, Atom property, Atom type,
                                unsigned long *count) {
    Atom actual = None;
    int format = 0;
    unsigned long after = 0;
    unsigned char *data = NULL;

    *count = 0;
    if (XGetWindowProperty(xdisplay, window, property, 0, 1024, False, type,
                           &actual, &format, count, &after, &data) != Success) {
        *count = 0;
        return NULL;
    }
    if (data != NULL && (actual != type || format != 32)) {
        XFree(data);
        *count = 0;
        return NULL;
    }
    return (unsigned long *)data;
}

static gboolean has_atom(Window window, Atom property, Atom wanted) {
    unsigned long count, i;
    unsigned long *list = get_longs(window, property, XA_ATOM, &count);
    gboolean found = FALSE;

    for (i = 0; i < count && !found; i++) {
        found = (list[i] == wanted);
    }
    if (list != NULL) {
        XFree(list);
    }
    return found;
}

// Copies text and cuts it at the first invalid UTF8 byte, so GTK never
// receives a string broken by truncation or a Latin1 fallback name.
static void copy_title(char *buffer, int size, const char *text) {
    const gchar *end = NULL;

    snprintf(buffer, size, "%s", text);
    if (!g_utf8_validate(buffer, (gssize)strlen(buffer), &end)) {
        *(gchar *)end = '\0';
    }
}

static void get_title(Window window, char *buffer, int size) {
    char *name = NULL;
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;

    buffer[0] = '\0';
    if (XGetWindowProperty(xdisplay, window, atom_name, 0, TITLE_LEN, False,
                           atom_utf8, &type, &format, &count, &after,
                           &data) == Success && data != NULL) {
        copy_title(buffer, size, (char *)data);
        XFree(data);
    }
    if (buffer[0] != '\0') {
        return;
    }
    if (XFetchName(xdisplay, window, &name) && name != NULL) {
        copy_title(buffer, size, name);
        XFree(name);
    }
}

static unsigned long current_active(void) {
    unsigned long count;
    unsigned long *active = get_longs(xroot, atom_active_window, XA_WINDOW, &count);
    unsigned long value = 0;

    if (active != NULL) {
        if (count > 0) {
            value = active[0];
        }
        XFree(active);
    }
    return value;
}

// Skips docks, desktops and windows that ask not to be on the taskbar.
static gboolean wants_taskbar_entry(Window window) {
    return !has_atom(window, atom_net_state, atom_state_skip_taskbar) &&
           !has_atom(window, atom_window_type, atom_type_dock) &&
           !has_atom(window, atom_window_type, atom_type_desktop);
}

// process launching

static void child_setup(gpointer data) {
    (void)data;
    setsid();
}

static void spawn(const char *path) {
    char *argv[2];
    GError *error = NULL;

    argv[0] = (char *)path;
    argv[1] = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, child_setup,
                       NULL, NULL, &error)) {
        g_warning("failed to launch %s: %s", path, (*error).message);
        g_error_free(error);
    }
}

// callbacks

static void on_show_desktop(GtkButton *button, gpointer data) {
    unsigned long count;
    unsigned long *showing = get_longs(xroot, atom_showing_desktop, XA_CARDINAL, &count);
    long next = 1;

    (void)button;
    (void)data;
    if (showing != NULL) {
        if (count > 0 && showing[0] != 0) {
            next = 0;
        }
        XFree(showing);
    }
    send_message(xroot, atom_showing_desktop, next);
}

static void on_launch(GtkButton *button, gpointer data) {
    (void)button;
    spawn((const char *)data);
}

static void on_workspace(GtkButton *button, gpointer data) {
    (void)button;
    send_message2(xroot, atom_current_desktop, (long)GPOINTER_TO_INT(data), CurrentTime);
}

static void on_task_clicked(GtkButton *button, gpointer data) {
    Window window = (Window)GPOINTER_TO_SIZE(data);

    (void)button;
    if (window == current_active() &&
        !has_atom(window, atom_net_state, atom_state_hidden)) {
        send_message(window, atom_change_state, IconicState);
    } else {
        // source indication 2 means a pager or taskbar
        send_message2(window, atom_active_window, 2, CurrentTime);
    }
}

static void launch_app(GtkButton *button, gpointer data) {
    GdkAppLaunchContext *context =
        gdk_display_get_app_launch_context(gtk_widget_get_display(GTK_WIDGET(button)));
    GError *error = NULL;

    gtk_popover_popdown(GTK_POPOVER(popover));
    if (!g_app_info_launch(G_APP_INFO(data), NULL, G_APP_LAUNCH_CONTEXT(context), &error)) {
        g_warning("launch failed: %s", (*error).message);
        g_error_free(error);
    }
    g_object_unref(context);
}

// taskbar

static void rebuild_taskbar(const Task *tasks, int count) {
    GtkWidget *child;
    int i;

    while ((child = gtk_widget_get_first_child(taskbar)) != NULL) {
        gtk_box_remove(GTK_BOX(taskbar), child);
    }
    for (i = 0; i < count; i++) {
        GtkWidget *button = gtk_button_new();
        GtkWidget *label;
        gchar *text;

        if (tasks[i].hidden) {
            text = g_strdup_printf("(%s)", tasks[i].title);
        } else if (tasks[i].active) {
            text = g_strdup_printf("[%s]", tasks[i].title);
        } else {
            text = g_strdup(tasks[i].title);
        }
        label = gtk_label_new(text);
        g_free(text);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 18);
        gtk_button_set_child(GTK_BUTTON(button), label);
        gtk_widget_set_tooltip_text(button, tasks[i].title);
        g_signal_connect(button, "clicked", G_CALLBACK(on_task_clicked),
                         GSIZE_TO_POINTER((gsize)tasks[i].window));
        gtk_box_append(GTK_BOX(taskbar), button);
    }
}

static void update_workspaces(void) {
    unsigned long count;
    unsigned long *desktop = get_longs(xroot, atom_current_desktop, XA_CARDINAL, &count);
    int index;

    if (desktop == NULL) {
        return;
    }
    if (count > 0 && (int)desktop[0] != last_current) {
        last_current = (int)desktop[0];
        for (index = 0; index < WORKSPACES; index++) {
            gchar *text = (index == last_current)
                              ? g_strdup_printf("[%d]", index + 1)
                              : g_strdup_printf("%d", index + 1);
            gtk_button_set_label(GTK_BUTTON(workspace_buttons[index]), text);
            g_free(text);
        }
    }
    XFree(desktop);
}

static gboolean poll_windows(gpointer data) {
    unsigned long count;
    unsigned long *list = get_longs(xroot, atom_client_list, XA_WINDOW, &count);
    unsigned long active_window = current_active();
    Task tasks[MAX_TASKS];
    int n = 0;
    unsigned long i;
    GString *signature = g_string_new("");

    (void)data;
    for (i = 0; i < count && n < MAX_TASKS; i++) {
        if (!wants_taskbar_entry(list[i])) {
            continue;
        }
        tasks[n].window = list[i];
        tasks[n].active = (list[i] == active_window);
        tasks[n].hidden = has_atom(list[i], atom_net_state, atom_state_hidden);
        get_title(list[i], tasks[n].title, sizeof(tasks[n].title));
        g_string_append_printf(signature, "%lu:%d:%d:%s;", (unsigned long)tasks[n].window,
                               tasks[n].active, tasks[n].hidden, tasks[n].title);
        n++;
    }
    if (g_strcmp0((*signature).str, last_signature) != 0) {
        g_free(last_signature);
        last_signature = g_string_free(signature, FALSE);
        rebuild_taskbar(tasks, n);
    } else {
        g_string_free(signature, TRUE);
    }
    update_workspaces();

    if (list != NULL) {
        XFree(list);
    }
    return G_SOURCE_CONTINUE;
}

// status area

static gboolean read_trimmed(const char *path, gchar **out) {
    if (!g_file_get_contents(path, out, NULL, NULL)) {
        return FALSE;
    }
    g_strstrip(*out);
    return TRUE;
}

static gboolean update_status(gpointer data) {
    char text[16];
    time_t now = time(NULL);
    struct tm local;
    gchar *capacity = NULL;
    gchar *state = NULL;

    (void)data;
    localtime_r(&now, &local);
    strftime(text, sizeof(text), "%H:%M:%S", &local);
    gtk_label_set_text(GTK_LABEL(clock_label), text);

    if (read_trimmed("/sys/class/power_supply/BAT0/capacity", &capacity)) {
        gchar *label;

        read_trimmed("/sys/class/power_supply/BAT0/status", &state);
        label = g_strdup_printf("BAT %s%% %s", capacity, state != NULL ? state : "");
        g_strstrip(label);
        gtk_label_set_text(GTK_LABEL(battery_label), label);
        g_free(label);
    } else {
        gtk_label_set_text(GTK_LABEL(battery_label), "AC");
    }
    g_free(capacity);
    g_free(state);
    return G_SOURCE_CONTINUE;
}

// application menu

static gint compare_apps(gconstpointer a, gconstpointer b) {
    return g_utf8_collate(g_app_info_get_name((GAppInfo *)a),
                          g_app_info_get_name((GAppInfo *)b));
}

static GtkWidget *build_popover(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GList *apps = g_list_sort(g_app_info_get_all(), compare_apps);
    GList *node;

    for (node = apps; node != NULL; node = g_list_next(node)) {
        GAppInfo *info = (*node).data;
        GtkWidget *button;

        if (!g_app_info_should_show(info)) {
            continue;
        }
        button = gtk_button_new_with_label(g_app_info_get_name(info));
        // the button holds its own reference to info
        g_signal_connect_data(button, "clicked", G_CALLBACK(launch_app),
                              g_object_ref(info), (GClosureNotify)g_object_unref, 0);
        gtk_box_append(GTK_BOX(box), button);
    }
    g_list_free_full(apps, g_object_unref);

    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 400);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 260);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), scroll);
    return popover;
}

static GtkWidget *add_launcher(GtkWidget *bar, const char *label, const char *path) {
    GtkWidget *button = gtk_button_new_with_label(label);

    g_signal_connect(button, "clicked", G_CALLBACK(on_launch), (gpointer)path);
    gtk_box_append(GTK_BOX(bar), button);
    return button;
}

// dock behaviour

// Turns the window into a panel: dock type, reserved screen edge, full width.
static void on_realize(GtkWidget *widget, gpointer data) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    int screen = DefaultScreen(xdisplay);
    int width = DisplayWidth(xdisplay, screen);
    int height = DisplayHeight(xdisplay, screen);
    long strut[12] = {0};
    Atom type = atom_type_dock;
    Window xid;

    (void)data;
    if (!GDK_IS_X11_SURFACE(surface)) {
        return;
    }
    xid = gdk_x11_surface_get_xid(surface);

    XChangeProperty(xdisplay, xid, atom_window_type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&type, 1);
    if (PANEL_BOTTOM) {
        strut[3] = PANEL_HEIGHT;
        strut[10] = 0;
        strut[11] = subtract(width, 1);
    } else {
        strut[2] = PANEL_HEIGHT;
        strut[8] = 0;
        strut[9] = subtract(width, 1);
    }
    XChangeProperty(xdisplay, xid, atom_strut_partial, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut, 12);
    XMoveResizeWindow(xdisplay, xid, 0, PANEL_BOTTOM ? subtract(height, PANEL_HEIGHT) : 0,
                      width, PANEL_HEIGHT);
    XFlush(xdisplay);
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *apps_button = gtk_menu_button_new();
    GtkWidget *show_button = gtk_button_new_with_label("Desktop");
    char text[8];
    int i;

    (void)data;
    gtk_window_set_title(GTK_WINDOW(window), "panel_de");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window),
                                DisplayWidth(xdisplay, DefaultScreen(xdisplay)),
                                PANEL_HEIGHT);
    g_signal_connect(window, "realize", G_CALLBACK(on_realize), NULL);

    gtk_menu_button_set_label(GTK_MENU_BUTTON(apps_button), "Applications");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(apps_button), build_popover());
    gtk_box_append(GTK_BOX(bar), apps_button);

    add_launcher(bar, "Terminal", "./terminal");
    add_launcher(bar, "Files", "./files");
    g_signal_connect(show_button, "clicked", G_CALLBACK(on_show_desktop), NULL);
    gtk_box_append(GTK_BOX(bar), show_button);

    for (i = 0; i < WORKSPACES; i++) {
        snprintf(text, sizeof(text), "%d", i + 1);
        workspace_buttons[i] = gtk_button_new_with_label(text);
        g_signal_connect(workspace_buttons[i], "clicked", G_CALLBACK(on_workspace),
                         GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(bar), workspace_buttons[i]);
    }

    taskbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(taskbar, TRUE);
    gtk_box_append(GTK_BOX(bar), taskbar);

    battery_label = gtk_label_new("");
    clock_label = gtk_label_new("");
    gtk_box_append(GTK_BOX(bar), battery_label);
    gtk_box_append(GTK_BOX(bar), clock_label);
    add_launcher(bar, "Settings", "./settings");
    add_launcher(bar, "Session", "./session");

    gtk_window_set_child(GTK_WINDOW(window), bar);
    update_status(NULL);
    poll_windows(NULL);
    g_timeout_add_seconds(1, update_status, NULL);
    g_timeout_add(500, poll_windows, NULL);
    gtk_window_present(GTK_WINDOW(window));
}

// main

static void intern_atoms(void) {
#define ATOM(var, name) var = XInternAtom(xdisplay, name, False)
    ATOM(atom_client_list, "_NET_CLIENT_LIST");
    ATOM(atom_active_window, "_NET_ACTIVE_WINDOW");
    ATOM(atom_current_desktop, "_NET_CURRENT_DESKTOP");
    ATOM(atom_name, "_NET_WM_NAME");
    ATOM(atom_utf8, "UTF8_STRING");
    ATOM(atom_net_state, "_NET_WM_STATE");
    ATOM(atom_state_hidden, "_NET_WM_STATE_HIDDEN");
    ATOM(atom_state_skip_taskbar, "_NET_WM_STATE_SKIP_TASKBAR");
    ATOM(atom_change_state, "WM_CHANGE_STATE");
    ATOM(atom_showing_desktop, "_NET_SHOWING_DESKTOP");
    ATOM(atom_window_type, "_NET_WM_WINDOW_TYPE");
    ATOM(atom_type_dock, "_NET_WM_WINDOW_TYPE_DOCK");
    ATOM(atom_type_desktop, "_NET_WM_WINDOW_TYPE_DESKTOP");
    ATOM(atom_strut_partial, "_NET_WM_STRUT_PARTIAL");
#undef ATOM
}

int main(int argc, char **argv) {
    GtkApplication *app;
    int status;

    gdk_set_allowed_backends("x11");
    if (!gtk_init_check()) {
        fprintf(stderr, "panel: cannot open display\n");
        return 1;
    }

    // share the connection GDK already opened instead of making a second one
    xdisplay = gdk_x11_display_get_xdisplay(gdk_display_get_default());
    xroot = DefaultRootWindow(xdisplay);
    previous_handler = XSetErrorHandler(handle_error);
    intern_atoms();

    app = gtk_application_new("de.bare.panel", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    g_free(last_signature);
    return status;
}