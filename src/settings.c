#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUTPUT_SIZE 2048
#define READ_LIMIT 2000

static GtkWidget *network_label;
static GtkWidget *battery_label;
static GtkWidget *ssid_entry;
static GtkWidget *password_entry;

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

static void on_volume_changed(GtkRange *range, gpointer data) {
    char text[16];
    char *arguments[5];

    snprintf(text, sizeof(text), "%d%%", (int)gtk_range_get_value(range));
    arguments[0] = "amixer";
    arguments[1] = "sset";
    arguments[2] = "Master";
    arguments[3] = text;
    arguments[4] = NULL;
    run(arguments);
}

static void on_mute(GtkButton *button, gpointer data) {
    char *arguments[5] = {"amixer", "sset", "Master", "toggle", NULL};
    run(arguments);
}

static void on_brightness_changed(GtkRange *range, gpointer data) {
    char text[16];
    char *arguments[4];
    int value = (int)gtk_range_get_value(range);

    if (value < 5) {
        value = 5;
    }
    snprintf(text, sizeof(text), "%d%%", value);
    arguments[0] = "brightnessctl";
    arguments[1] = "set";
    arguments[2] = text;
    arguments[3] = NULL;
    run(arguments);
}

static void on_wifi(GtkButton *button, gpointer data) {
    char *arguments[5];

    arguments[0] = "nmcli";
    arguments[1] = "radio";
    arguments[2] = "wifi";
    arguments[3] = (char *)data;
    arguments[4] = NULL;
    run(arguments);
}

static void on_refresh_network(GtkButton *button, gpointer data) {
    char status[OUTPUT_SIZE];
    char list[OUTPUT_SIZE];
    gchar *text;

    read_output("nmcli device status", status);
    read_output("nmcli device wifi list", list);
    text = g_strdup_printf("%s\n%s", status, list);
    gtk_label_set_text(GTK_LABEL(network_label), text);
    g_free(text);
}

static void on_connect(GtkButton *button, gpointer data) {
    const char *ssid = gtk_editable_get_text(GTK_EDITABLE(ssid_entry));
    const char *password = gtk_editable_get_text(GTK_EDITABLE(password_entry));
    char *arguments[8];

    if (ssid[0] == '\0') {
        return;
    }
    arguments[0] = "nmcli";
    arguments[1] = "device";
    arguments[2] = "wifi";
    arguments[3] = "connect";
    arguments[4] = (char *)ssid;
    if (password[0] != '\0') {
        arguments[5] = "password";
        arguments[6] = (char *)password;
        arguments[7] = NULL;
    } else {
        arguments[5] = NULL;
    }
    run(arguments);
}

static void on_wallpaper_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    gchar *path;
    gchar *directory;
    gchar *config;

    if (file == NULL) {
        return;
    }
    path = g_file_get_path(file);
    directory = g_build_filename(g_get_user_config_dir(), "de", NULL);
    config = g_build_filename(directory, "wallpaper", NULL);
    g_mkdir_with_parents(directory, 0755);
    if (path != NULL) {
        g_file_set_contents(config, path, (gssize)strlen(path), NULL);
    }
    g_free(path);
    g_free(directory);
    g_free(config);
    g_object_unref(file);
}

static void on_wallpaper_clicked(GtkButton *button, gpointer data) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_open(dialog, GTK_WINDOW(data), NULL, on_wallpaper_chosen, NULL);
    g_object_unref(dialog);
}

static void update_battery(void) {
    gchar *capacity = NULL;
    gchar *state = NULL;
    gchar *text;

    if (g_file_get_contents("/sys/class/power_supply/BAT0/capacity", &capacity, NULL, NULL)) {
        g_file_get_contents("/sys/class/power_supply/BAT0/status", &state, NULL, NULL);
        g_strstrip(capacity);
        if (state != NULL) {
            g_strstrip(state);
        }
        text = g_strdup_printf("Battery %s%% %s", capacity, state != NULL ? state : "");
        gtk_label_set_text(GTK_LABEL(battery_label), text);
        g_free(text);
    } else {
        gtk_label_set_text(GTK_LABEL(battery_label), "No battery");
    }
    g_free(capacity);
    g_free(state);
}

static GtkWidget *make_scale(int value, GCallback callback) {
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_range_set_value(GTK_RANGE(scale), value);
    g_signal_connect(scale, "value_changed", callback, NULL);
    return scale;
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *volume_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *brightness_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *wifi_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *connect_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *mute_button = gtk_button_new_with_label("Mute");
    GtkWidget *wifi_on = gtk_button_new_with_label("Wifi On");
    GtkWidget *wifi_off = gtk_button_new_with_label("Wifi Off");
    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
    GtkWidget *connect_button = gtk_button_new_with_label("Connect");
    GtkWidget *wallpaper_button = gtk_button_new_with_label("Wallpaper");

    gtk_window_set_title(GTK_WINDOW(window), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 480);

    gtk_box_append(GTK_BOX(volume_row), gtk_label_new("Volume"));
    gtk_box_append(GTK_BOX(volume_row), make_scale(get_volume(), G_CALLBACK(on_volume_changed)));
    gtk_box_append(GTK_BOX(volume_row), mute_button);
    gtk_box_append(GTK_BOX(brightness_row), gtk_label_new("Brightness"));
    gtk_box_append(GTK_BOX(brightness_row),
                   make_scale(get_brightness(), G_CALLBACK(on_brightness_changed)));

    gtk_box_append(GTK_BOX(wifi_row), wifi_on);
    gtk_box_append(GTK_BOX(wifi_row), wifi_off);
    gtk_box_append(GTK_BOX(wifi_row), refresh_button);

    ssid_entry = gtk_entry_new();
    password_entry = gtk_password_entry_new();
    gtk_widget_set_hexpand(ssid_entry, TRUE);
    gtk_widget_set_hexpand(password_entry, TRUE);
    gtk_box_append(GTK_BOX(connect_row), ssid_entry);
    gtk_box_append(GTK_BOX(connect_row), password_entry);
    gtk_box_append(GTK_BOX(connect_row), connect_button);

    network_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(network_label), 0);
    gtk_label_set_selectable(GTK_LABEL(network_label), TRUE);
    battery_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(battery_label), 0);

    g_signal_connect(mute_button, "clicked", G_CALLBACK(on_mute), NULL);
    g_signal_connect(wifi_on, "clicked", G_CALLBACK(on_wifi), "on");
    g_signal_connect(wifi_off, "clicked", G_CALLBACK(on_wifi), "off");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_network), NULL);
    g_signal_connect(connect_button, "clicked", G_CALLBACK(on_connect), NULL);
    g_signal_connect(wallpaper_button, "clicked", G_CALLBACK(on_wallpaper_clicked), window);

    gtk_box_append(GTK_BOX(box), volume_row);
    gtk_box_append(GTK_BOX(box), brightness_row);
    gtk_box_append(GTK_BOX(box), wifi_row);
    gtk_box_append(GTK_BOX(box), connect_row);
    gtk_box_append(GTK_BOX(box), network_label);
    gtk_box_append(GTK_BOX(box), battery_label);
    gtk_box_append(GTK_BOX(box), wallpaper_button);
    gtk_window_set_child(GTK_WINDOW(window), box);

    update_battery();
    on_refresh_network(NULL, NULL);
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
