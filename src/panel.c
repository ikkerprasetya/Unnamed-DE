#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKSPACES 4

static Display *xdisplay;
static Window xroot;
static Atom atom_client_list;
static Atom atom_active_window;
static Atom atom_current_desktop;
static Atom atom_name;
static Atom atom_utf8;
static Atom atom_net_state;
static Atom atom_state_hidden;
static Atom atom_change_state;
static Atom atom_showing_desktop;
static GtkWidget *clock_label;
static GtkWidget *battery_label;
static GtkWidget *popover;
static GtkWidget *taskbar;
static GtkWidget *workspace_buttons[WORKSPACES];
static gchar *last_signature;
static int last_current = WORKSPACES;

static int ignore_error(Display *d, XErrorEvent *e) {
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

static void send_message2(Window target, Atom type, long first, long second) {
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = target;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = first;
    event.xclient.data.l[1] = second;
    XSendEvent(xdisplay, xroot, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(xdisplay);
}

static void send_message(Window target, Atom type, long value) {
    send_message2(target, type, value, 0);
}

static unsigned long *get_longs(Window window, Atom property, Atom type, unsigned long *count) {
    Atom actual;
    int format;
    unsigned long after;
    unsigned char *data = NULL;

    *count = 0;
    if (XGetWindowProperty(xdisplay, window, property, 0, 1024, False, type,
                           &actual, &format, count, &after, &data) != Success) {
        *count = 0;
        return NULL;
    }
    return (unsigned long *)data;
}

static void get_title(Window window, char *buffer, int size) {
    char *name = NULL;
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;

    buffer[0] = '\0';
    if (XGetWindowProperty(xdisplay, window, atom_name, 0, 128, False, atom_utf8,
                           &type, &format, &count, &after, &data) == Success && data != NULL) {
        snprintf(buffer, size, "%s", (char *)data);
        XFree(data);
    }
    if (buffer[0] != '\0') {
        return;
    }
    if (XFetchName(xdisplay, window, &name) && name != NULL) {
        snprintf(buffer, size, "%s", name);
        XFree(name);
    }
}

static int is_hidden(Window window) {
    unsigned long count = 0;
    unsigned long i;
    int hidden = 0;
    unsigned long *list = get_longs(window, atom_net_state, XA_ATOM, &count);

    for (i = 0; i < count; i++) {
        if (list[i] == atom_state_hidden) {
            hidden = 1;
        }
    }
    if (list != NULL) {
        XFree(list);
    }
    return hidden;
}

static unsigned long current_active(void) {
    unsigned long count = 0;
    unsigned long *active = get_longs(xroot, atom_active_window, XA_WINDOW, &count);
    unsigned long value = 0;

    if (active != NULL && count > 0) {
        value = active[0];
    }
    if (active != NULL) {
        XFree(active);
    }
    return value;
}

static void on_show_desktop(GtkButton *button, gpointer data) {
    unsigned long count = 0;
    unsigned long *showing = get_longs(xroot, atom_showing_desktop, XA_CARDINAL, &count);
    long next = 1;

    if (showing != NULL && count > 0 && showing[0] != 0) {
        next = 0;
    }
    if (showing != NULL) {
        XFree(showing);
    }
    send_message(xroot, atom_showing_desktop, next);
}

static void on_launch(GtkButton *button, gpointer data) {
    spawn((const char *)data);
}

static void on_workspace(GtkButton *button, gpointer data) {
    send_message(xroot, atom_current_desktop, (long)GPOINTER_TO_INT(data));
}

static void on_task_clicked(GtkButton *button, gpointer data) {
    Window window = (Window)GPOINTER_TO_SIZE(data);

    if (window == current_active() && !is_hidden(window)) {
        send_message(window, atom_change_state, 3);
    } else {
        send_message(window, atom_active_window, 0);
    }
}

static void launch_app(GtkButton *button, gpointer data) {
    gtk_popover_popdown(GTK_POPOVER(popover));
    g_app_info_launch(G_APP_INFO(data), NULL, NULL, NULL);
}

static void rebuild_taskbar(unsigned long *list, unsigned long count, unsigned long active_window) {
    GtkWidget *child;
    unsigned long i;
    char title[128];

    while ((child = gtk_widget_get_first_child(taskbar)) != NULL) {
        gtk_box_remove(GTK_BOX(taskbar), child);
    }
    for (i = 0; i < count; i++) {
        GtkWidget *button = gtk_button_new();
        GtkWidget *label;
        gchar *text;

        get_title(list[i], title, sizeof(title));
        if (is_hidden(list[i])) {
            text = g_strdup_printf("(%s)", title);
        } else if (list[i] == active_window) {
            text = g_strdup_printf("[%s]", title);
        } else {
            text = g_strdup(title);
        }
        label = gtk_label_new(text);
        g_free(text);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 18);
        gtk_button_set_child(GTK_BUTTON(button), label);
        g_signal_connect(button, "clicked", G_CALLBACK(on_task_clicked),
                         GSIZE_TO_POINTER((gsize)list[i]));
        gtk_box_append(GTK_BOX(taskbar), button);
    }
}

static gboolean poll_windows(gpointer data) {
    unsigned long count = 0;
    unsigned long active_count = 0;
    unsigned long current_count = 0;
    unsigned long *list = get_longs(xroot, atom_client_list, XA_WINDOW, &count);
    unsigned long *active = get_longs(xroot, atom_active_window, XA_WINDOW, &active_count);
    unsigned long *desktop = get_longs(xroot, atom_current_desktop, XA_CARDINAL, &current_count);
    unsigned long active_window = 0;
    unsigned long i;
    char title[128];
    GString *signature = g_string_new("");
    gchar *text_signature;
    int index;

    if (active != NULL && active_count > 0) {
        active_window = active[0];
    }
    for (i = 0; i < count; i++) {
        get_title(list[i], title, sizeof(title));
        g_string_append_printf(signature, "%lu:%d:%d:%s;", list[i],
                               list[i] == active_window, is_hidden(list[i]), title);
    }
    text_signature = g_string_free(signature, FALSE);
    if (g_strcmp0(text_signature, last_signature) != 0) {
        g_free(last_signature);
        last_signature = text_signature;
        rebuild_taskbar(list, count, active_window);
    } else {
        g_free(text_signature);
    }

    if (desktop != NULL && current_count > 0 && (int)desktop[0] != last_current) {
        last_current = (int)desktop[0];
        for (index = 0; index < WORKSPACES; index++) {
            gchar *text;
            if (index == last_current) {
                text = g_strdup_printf("[%d]", index + 1);
            } else {
                text = g_strdup_printf("%d", index + 1);
            }
            gtk_button_set_label(GTK_BUTTON(workspace_buttons[index]), text);
            g_free(text);
        }
    }

    if (list != NULL) {
        XFree(list);
    }
    if (active != NULL) {
        XFree(active);
    }
    if (desktop != NULL) {
        XFree(desktop);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean update_status(gpointer data) {
    char text[16];
    time_t now = time(NULL);
    struct tm local;
    gchar *capacity = NULL;
    gchar *state = NULL;
    gchar *label;

    localtime_r(&now, &local);
    strftime(text, sizeof(text), "%H:%M:%S", &local);
    gtk_label_set_text(GTK_LABEL(clock_label), text);

    if (g_file_get_contents("/sys/class/power_supply/BAT0/capacity", &capacity, NULL, NULL)) {
        g_file_get_contents("/sys/class/power_supply/BAT0/status", &state, NULL, NULL);
        g_strstrip(capacity);
        if (state != NULL) {
            g_strstrip(state);
        }
        label = g_strdup_printf("BAT %s%% %s", capacity, state != NULL ? state : "");
        gtk_label_set_text(GTK_LABEL(battery_label), label);
        g_free(label);
    } else {
        gtk_label_set_text(GTK_LABEL(battery_label), "AC");
    }
    g_free(capacity);
    g_free(state);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *build_popover(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GList *list = g_app_info_get_all();
    guint total = g_list_length(list);
    guint i;

    for (i = 0; i < total; i++) {
        GAppInfo *info = g_list_nth_data(list, i);
        GtkWidget *button;
        if (!g_app_info_should_show(info)) {
            continue;
        }
        button = gtk_button_new_with_label(g_app_info_get_name(info));
        g_signal_connect(button, "clicked", G_CALLBACK(launch_app), info);
        gtk_box_append(GTK_BOX(box), button);
    }
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

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *apps_button = gtk_menu_button_new();
    char text[8];
    int i;

    gtk_window_set_title(GTK_WINDOW(window), "panel_de");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 32);

    gtk_menu_button_set_label(GTK_MENU_BUTTON(apps_button), "Applications");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(apps_button), build_popover());
    gtk_box_append(GTK_BOX(bar), apps_button);

    add_launcher(bar, "Terminal", "./terminal");
    add_launcher(bar, "Files", "./files");
    {
        GtkWidget *show_button = gtk_button_new_with_label("Desktop");
        g_signal_connect(show_button, "clicked", G_CALLBACK(on_show_desktop), NULL);
        gtk_box_append(GTK_BOX(bar), show_button);
    }

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

int main(int argc, char **argv) {
    GtkApplication *app;
    int status;

    xdisplay = XOpenDisplay(NULL);
    if (xdisplay == NULL) {
        return 1;
    }
    xroot = DefaultRootWindow(xdisplay);
    XSetErrorHandler(ignore_error);
    atom_client_list = XInternAtom(xdisplay, "_NET_CLIENT_LIST", False);
    atom_active_window = XInternAtom(xdisplay, "_NET_ACTIVE_WINDOW", False);
    atom_current_desktop = XInternAtom(xdisplay, "_NET_CURRENT_DESKTOP", False);
    atom_name = XInternAtom(xdisplay, "_NET_WM_NAME", False);
    atom_utf8 = XInternAtom(xdisplay, "UTF8_STRING", False);
    atom_net_state = XInternAtom(xdisplay, "_NET_WM_STATE", False);
    atom_state_hidden = XInternAtom(xdisplay, "_NET_WM_STATE_HIDDEN", False);
    atom_change_state = XInternAtom(xdisplay, "WM_CHANGE_STATE", False);
    atom_showing_desktop = XInternAtom(xdisplay, "_NET_SHOWING_DESKTOP", False);

    app = gtk_application_new("de.bare.panel", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
