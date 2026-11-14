#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUTPUT_SIZE 2048
#define READ_LIMIT 2000
#define APPLY_DELAY 120
#define REFRESH_DELAY 2
#define BATTERY_SECONDS 5
#define POWER_DIRECTORY "/sys/class/power_supply"
#define DASH 45

static GtkWidget *window;
static GtkWidget *network_list;
static GtkWidget *battery_label;
static GtkWidget *status_label;
static GtkWidget *mute_button;
static GtkWidget *connect_button;
static GtkWidget *ssid_entry;
static GtkWidget *password_entry;
static gchar *battery_path;
static guint volume_timer;
static guint brightness_timer;
static int volume_value;
static int brightness_value;

static void run(char *const arguments[]) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execvp(arguments[0], arguments);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static void read_output(const char *command, char *buffer) {
    FILE *pipe = popen(command, "r");
    size_t count;

    buffer[0] = '\0';
    if (pipe == NULL) {
        return;
    }
    count = fread(buffer, 1, READ_LIMIT, pipe);
    buffer[count] = '\0';
    pclose(pipe);
}

static gchar *read_text(const gchar *directory, const gchar *name) {
    gchar *file = g_build_filename(directory, name, NULL);
    gchar *text = NULL;

    if (g_file_get_contents(file, &text, NULL, NULL)) {
        g_strstrip(text);
    }
    g_free(file);
    return text;
}

static gboolean start_command(const gchar *const arguments[], GAsyncReadyCallback callback) {
    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(arguments,
                                             G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                             G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                             &error);

    if (process == NULL) {
        g_clear_error(&error);
        gtk_label_set_text(GTK_LABEL(status_label), "Command not found");
        return FALSE;
    }
    g_subprocess_communicate_utf8_async(process, NULL, NULL, callback, NULL);
    return TRUE;
}

static gchar *collect(GObject *source, GAsyncResult *result, gboolean *success) {
    gchar *output = NULL;
    gchar *errors = NULL;
    GError *error = NULL;
    gchar *text;
    gboolean finished;

    finished = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                                    &output, &errors, &error);
    g_clear_error(&error);
    *success = finished && g_subprocess_get_successful(G_SUBPROCESS(source));
    if (*success) {
        text = output;
        g_free(errors);
    } else {
        text = errors;
        g_free(output);
    }
    if (text == NULL) {
        text = g_strdup("");
    }
    g_strstrip(text);
    g_object_unref(source);
    return text;
}

static int get_volume(void) {
    char output[OUTPUT_SIZE];
    char *start;

    read_output("amixer sget Master", output);
    start = strchr(output, '[');
    if (start == NULL) {
        return 50;
    }
    return atoi(start + 1);
}

static int get_brightness(void) {
    char current[OUTPUT_SIZE];
    char maximum[OUTPUT_SIZE];
    int top;

    read_output("brightnessctl get", current);
    read_output("brightnessctl max", maximum);
    top = atoi(maximum);
    if (top <= 0) {
        return 50;
    }
    return atoi(current) * 100 / top;
}

static void show_mute(const char *output) {
    if (strstr(output, "[off]") != NULL) {
        gtk_button_set_label(GTK_BUTTON(mute_button), "Unmute");
    } else {
        gtk_button_set_label(GTK_BUTTON(mute_button), "Mute");
    }
}

static gboolean apply_volume(gpointer data) {
    char text[16];
    char *arguments[5];

    volume_timer = 0;
    snprintf(text, sizeof(text), "%d%%", volume_value);
    arguments[0] = "amixer";
    arguments[1] = "sset";
    arguments[2] = "Master";
    arguments[3] = text;
    arguments[4] = NULL;
    run(arguments);
    return G_SOURCE_REMOVE;
}

static void on_volume_changed(GtkRange *range, gpointer data) {
    volume_value = (int)gtk_range_get_value(range);
    if (volume_timer != 0) {
        g_source_remove(volume_timer);
    }
    volume_timer = g_timeout_add(APPLY_DELAY, apply_volume, NULL);
}

static void on_mute(GtkButton *button, gpointer data) {
    char output[OUTPUT_SIZE];

    read_output("amixer sset Master toggle", output);
    show_mute(output);
}

static gboolean apply_brightness(gpointer data) {
    char text[16];
    char *arguments[4];

    brightness_timer = 0;
    snprintf(text, sizeof(text), "%d%%", brightness_value);
    arguments[0] = "brightnessctl";
    arguments[1] = "set";
    arguments[2] = text;
    arguments[3] = NULL;
    run(arguments);
    return G_SOURCE_REMOVE;
}

static void on_brightness_changed(GtkRange *range, gpointer data) {
    brightness_value = (int)gtk_range_get_value(range);
    if (brightness_value < 5) {
        brightness_value = 5;
    }
    if (brightness_timer != 0) {
        g_source_remove(brightness_timer);
    }
    brightness_timer = g_timeout_add(APPLY_DELAY, apply_brightness, NULL);
}

static int subtract(int a, int b) {
    return a + (~b + 1);
}

static int find_column(const gchar *header, const gchar *name) {
    size_t length = strlen(name);
    char previous = ' ';
    int i;

    for (i = 0; header[i] != '\0'; i++) {
        if (previous == ' ' && strncmp(header + i, name, length) == 0) {
            return i;
        }
        previous = header[i];
    }
    return G_MAXINT;
}

static gchar *column(const gchar *line, int start, int end) {
    int length = (int)strlen(line);
    gchar *text;

    if (start >= length) {
        return g_strdup("");
    }
    if (end > length) {
        end = length;
    }
    if (end <= start) {
        return g_strdup("");
    }
    text = g_strndup(line + start, (gsize)subtract(end, start));
    return g_strstrip(text);
}

static void add_row(const gchar *text, const gchar *ssid) {
    GtkWidget *label = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (ssid != NULL) {
        g_object_set_data_full(G_OBJECT(label), "ssid", g_strdup(ssid), g_free);
    }
    gtk_list_box_append(GTK_LIST_BOX(network_list), label);
}

static void on_refresh_done(GObject *source, GAsyncResult *result, gpointer data) {
    gboolean success;
    gchar *text = collect(source, result, &success);
    gchar **lines = g_strsplit(text, "\n", 0);
    GtkWidget *child;
    int ssid_start = G_MAXINT;
    int mode_start = G_MAXINT;
    int signal_start = G_MAXINT;
    int bars_start = G_MAXINT;
    int security_start = G_MAXINT;
    int count = 0;
    int i;

    while ((child = gtk_widget_get_first_child(network_list)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(network_list), child);
    }
    if (success && lines[0] != NULL) {
        ssid_start = find_column(lines[0], "SSID");
        mode_start = find_column(lines[0], "MODE");
        signal_start = find_column(lines[0], "SIGNAL");
        bars_start = find_column(lines[0], "BARS");
        security_start = find_column(lines[0], "SECURITY");
        for (i = 1; lines[i] != NULL; i++) {
            gchar *ssid = column(lines[i], ssid_start, mode_start);
            gchar *strength = column(lines[i], signal_start, bars_start);
            gchar *security = column(lines[i], security_start, G_MAXINT);
            gchar *row;

            if (ssid[0] != '\0' && !(ssid[0] == DASH && ssid[1] == DASH)) {
                row = g_strdup_printf("%s%s  %s%%  %s", lines[i][0] == '*' ? "* " : "  ",
                                      ssid, strength, security);
                add_row(row, ssid);
                g_free(row);
                count++;
            }
            g_free(ssid);
            g_free(strength);
            g_free(security);
        }
    }
    if (count == 0) {
        if (!success && text[0] != '\0') {
            add_row(text, NULL);
        } else {
            add_row("No networks found", NULL);
        }
    }
    g_strfreev(lines);
    g_free(text);
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    GtkWidget *child = gtk_list_box_row_get_child(row);
    const gchar *ssid = g_object_get_data(G_OBJECT(child), "ssid");

    if (ssid == NULL) {
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(ssid_entry), ssid);
    gtk_widget_grab_focus(password_entry);
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                       GdkModifierType state, gpointer data) {
    if (keyval == GDK_KEY_Escape) {
        gtk_window_destroy(GTK_WINDOW(window));
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void refresh_network(void) {
    const gchar *arguments[5] = {"nmcli", "device", "wifi", "list", NULL};
    start_command(arguments, on_refresh_done);
}

static void on_refresh_clicked(GtkButton *button, gpointer data) {
    refresh_network();
}

static gboolean delayed_refresh(gpointer data) {
    refresh_network();
    return G_SOURCE_REMOVE;
}

static void on_wifi(GtkButton *button, gpointer data) {
    char *arguments[5];

    arguments[0] = "nmcli";
    arguments[1] = "radio";
    arguments[2] = "wifi";
    arguments[3] = (char *)data;
    arguments[4] = NULL;
    run(arguments);
    g_timeout_add_seconds(REFRESH_DELAY, delayed_refresh, NULL);
}

static void on_connect_done(GObject *source, GAsyncResult *result, gpointer data) {
    gboolean success;
    gchar *text = collect(source, result, &success);

    if (text[0] != '\0') {
        gtk_label_set_text(GTK_LABEL(status_label), text);
    } else if (success) {
        gtk_label_set_text(GTK_LABEL(status_label), "Connected");
    } else {
        gtk_label_set_text(GTK_LABEL(status_label), "Connection failed");
    }
    g_free(text);
    gtk_widget_set_sensitive(connect_button, TRUE);
    refresh_network();
}

static void on_connect(GtkButton *button, gpointer data) {
    const gchar *ssid = gtk_editable_get_text(GTK_EDITABLE(ssid_entry));
    const gchar *password = gtk_editable_get_text(GTK_EDITABLE(password_entry));
    const gchar *arguments[8];

    if (ssid[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(status_label), "Enter a network name");
        return;
    }
    arguments[0] = "nmcli";
    arguments[1] = "device";
    arguments[2] = "wifi";
    arguments[3] = "connect";
    arguments[4] = ssid;
    if (password[0] != '\0') {
        arguments[5] = "password";
        arguments[6] = password;
        arguments[7] = NULL;
    } else {
        arguments[5] = NULL;
    }
    gtk_label_set_text(GTK_LABEL(status_label), "Connecting");
    gtk_widget_set_sensitive(connect_button, FALSE);
    if (!start_command(arguments, on_connect_done)) {
        gtk_widget_set_sensitive(connect_button, TRUE);
    }
}

static void on_enter(GtkWidget *widget, gpointer data) {
    on_connect(NULL, NULL);
}

static void on_wallpaper_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    gchar *path;
    gchar *directory;
    gchar *config;

    g_clear_error(&error);
    if (file == NULL) {
        return;
    }
    path = g_file_get_path(file);
    directory = g_build_filename(g_get_user_config_dir(), "de", NULL);
    config = g_build_filename(directory, "wallpaper", NULL);
    g_mkdir_with_parents(directory, 0755);
    if (path != NULL && g_file_set_contents(config, path, (gssize)strlen(path), NULL)) {
        gtk_label_set_text(GTK_LABEL(status_label), "Wallpaper updated");
    }
    g_free(path);
    g_free(directory);
    g_free(config);
    g_object_unref(file);
}

static void on_wallpaper_clicked(GtkButton *button, gpointer data) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_pixbuf_formats(filter);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, on_wallpaper_chosen, NULL);
    g_object_unref(filter);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static gchar *find_battery(void) {
    GDir *dir = g_dir_open(POWER_DIRECTORY, 0, NULL);
    const gchar *name;
    gchar *found = NULL;
    gchar *path;
    gchar *type;

    if (dir == NULL) {
        return NULL;
    }
    while (found == NULL && (name = g_dir_read_name(dir)) != NULL) {
        path = g_build_filename(POWER_DIRECTORY, name, NULL);
        type = read_text(path, "type");
        if (g_strcmp0(type, "Battery") == 0) {
            found = path;
        } else {
            g_free(path);
        }
        g_free(type);
    }
    g_dir_close(dir);
    return found;
}

static gboolean update_battery(gpointer data) {
    gchar *capacity;
    gchar *state;
    gchar *text;

    if (battery_path == NULL) {
        gtk_label_set_text(GTK_LABEL(battery_label), "No battery");
        return G_SOURCE_CONTINUE;
    }
    capacity = read_text(battery_path, "capacity");
    state = read_text(battery_path, "status");
    if (capacity == NULL) {
        text = g_strdup("No battery");
    } else {
        text = g_strdup_printf("Battery %s%% %s", capacity, state != NULL ? state : "");
    }
    gtk_label_set_text(GTK_LABEL(battery_label), text);
    g_free(text);
    g_free(capacity);
    g_free(state);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *make_scale(int value, GCallback callback) {
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_range_set_value(GTK_RANGE(scale), value);
    g_signal_connect(scale, "value_changed", callback, NULL);
    return scale;
}

static GtkWidget *make_title(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

static GtkWidget *make_row(void) {
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *volume_row = make_row();
    GtkWidget *brightness_row = make_row();
    GtkWidget *wifi_row = make_row();
    GtkWidget *connect_row = make_row();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *wifi_on = gtk_button_new_with_label("Wifi On");
    GtkWidget *wifi_off = gtk_button_new_with_label("Wifi Off");
    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
    GtkWidget *wallpaper_button = gtk_button_new_with_label("Choose Wallpaper");
    GtkWidget *volume_label = gtk_label_new("Volume");
    GtkWidget *brightness_label = gtk_label_new("Brightness");
    GtkSizeGroup *group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    GtkEventController *keys = gtk_event_controller_key_new();
    char output[OUTPUT_SIZE];

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Settings");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 520);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    mute_button = gtk_button_new_with_label("Mute");
    connect_button = gtk_button_new_with_label("Connect");
    ssid_entry = gtk_entry_new();
    password_entry = gtk_password_entry_new();
    network_list = gtk_list_box_new();
    battery_label = gtk_label_new("");
    status_label = gtk_label_new("");

    gtk_entry_set_placeholder_text(GTK_ENTRY(ssid_entry), "Network name");
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(password_entry), TRUE);
    gtk_widget_set_hexpand(ssid_entry, TRUE);
    gtk_widget_set_hexpand(password_entry, TRUE);
    gtk_label_set_xalign(GTK_LABEL(battery_label), 0);
    gtk_label_set_xalign(GTK_LABEL(status_label), 0);
    gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 160);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), network_list);

    gtk_label_set_xalign(GTK_LABEL(volume_label), 0);
    gtk_label_set_xalign(GTK_LABEL(brightness_label), 0);
    gtk_size_group_add_widget(group, volume_label);
    gtk_size_group_add_widget(group, brightness_label);
    gtk_box_append(GTK_BOX(volume_row), volume_label);
    gtk_box_append(GTK_BOX(volume_row), make_scale(get_volume(), G_CALLBACK(on_volume_changed)));
    gtk_box_append(GTK_BOX(volume_row), mute_button);
    gtk_box_append(GTK_BOX(brightness_row), brightness_label);
    gtk_box_append(GTK_BOX(brightness_row),
                   make_scale(get_brightness(), G_CALLBACK(on_brightness_changed)));
    gtk_box_append(GTK_BOX(wifi_row), wifi_on);
    gtk_box_append(GTK_BOX(wifi_row), wifi_off);
    gtk_box_append(GTK_BOX(wifi_row), refresh_button);
    gtk_box_append(GTK_BOX(connect_row), ssid_entry);
    gtk_box_append(GTK_BOX(connect_row), password_entry);
    gtk_box_append(GTK_BOX(connect_row), connect_button);

    g_signal_connect(mute_button, "clicked", G_CALLBACK(on_mute), NULL);
    g_signal_connect(wifi_on, "clicked", G_CALLBACK(on_wifi), "on");
    g_signal_connect(wifi_off, "clicked", G_CALLBACK(on_wifi), "off");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    g_signal_connect(connect_button, "clicked", G_CALLBACK(on_connect), NULL);
    g_signal_connect(password_entry, "activate", G_CALLBACK(on_enter), NULL);
    g_signal_connect(network_list, "row_activated", G_CALLBACK(on_row_activated), NULL);
    g_signal_connect(keys, "key_pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(window, keys);
    g_object_unref(group);
    g_signal_connect(wallpaper_button, "clicked", G_CALLBACK(on_wallpaper_clicked), NULL);

    gtk_box_append(GTK_BOX(box), make_title("Sound"));
    gtk_box_append(GTK_BOX(box), volume_row);
    gtk_box_append(GTK_BOX(box), make_title("Display"));
    gtk_box_append(GTK_BOX(box), brightness_row);
    gtk_box_append(GTK_BOX(box), make_title("Network"));
    gtk_box_append(GTK_BOX(box), wifi_row);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), connect_row);
    gtk_box_append(GTK_BOX(box), status_label);
    gtk_box_append(GTK_BOX(box), make_title("Power"));
    gtk_box_append(GTK_BOX(box), battery_label);
    gtk_box_append(GTK_BOX(box), make_title("Appearance"));
    gtk_box_append(GTK_BOX(box), wallpaper_button);
    gtk_window_set_child(GTK_WINDOW(window), box);

    read_output("amixer sget Master", output);
    show_mute(output);
    battery_path = find_battery();
    update_battery(NULL);
    g_timeout_add_seconds(BATTERY_SECONDS, update_battery, NULL);
    refresh_network();
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("de.bare.settings", G_APPLICATION_NON_UNIQUE);
    int status;

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}