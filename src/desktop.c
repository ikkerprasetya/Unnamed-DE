#include <gtk/gtk.h>

#define ICON_SIZE 48
#define BOX_SPACING 4
#define LABEL_CHARS 12
#define ROWS_PER_COLUMN 10
#define REFRESH_SECONDS 3
#define REFILL_DELAY_MS 150

static GtkWidget *picture;
static GtkWidget *flow;
static GFileMonitor *monitor;
static gchar *loaded_path;
static guint refill_source;

static gint compare_names(gconstpointer a, gconstpointer b) {
    return g_utf8_collate((const gchar *)a, (const gchar *)b);
}

static void free_uri(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static gboolean refresh_wallpaper(gpointer data) {
    gchar *file = g_build_filename(g_get_user_config_dir(), "de", "wallpaper", NULL);
    gchar *contents = NULL;

    (void)data;
    if (g_file_get_contents(file, &contents, NULL, NULL)) {
        g_strstrip(contents);
        if (contents[0] != '\0' && g_strcmp0(contents, loaded_path) != 0 &&
            g_file_test(contents, G_FILE_TEST_IS_REGULAR)) {
            g_free(loaded_path);
            loaded_path = g_strdup(contents);
            gtk_picture_set_filename(GTK_PICTURE(picture), loaded_path);
        }
        g_free(contents);
    }
    g_free(file);
    return G_SOURCE_CONTINUE;
}

static void open_entry(GtkButton *button, gpointer data) {
    GError *error = NULL;

    (void)button;
    if (!g_app_info_launch_default_for_uri((const char *)data, NULL, &error)) {
        g_warning("cannot open %s: %s", (const char *)data,
                  error != NULL ? (*error).message : "unknown error");
        g_clear_error(&error);
    }
}

static GtkWidget *build_entry(const gchar *directory, const gchar *name) {
    gchar *path = g_build_filename(directory, name, NULL);
    GFile *file = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(file,
                                        G_FILE_ATTRIBUTE_STANDARD_ICON ","
                                        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    GtkWidget *button = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, BOX_SPACING);
    GtkWidget *label;
    const gchar *text = name;

    if (info != NULL) {
        GIcon *icon = g_file_info_get_icon(info);
        const gchar *display = g_file_info_get_display_name(info);

        if (icon != NULL) {
            GtkWidget *image = gtk_image_new_from_gicon(icon);

            gtk_image_set_pixel_size(GTK_IMAGE(image), ICON_SIZE);
            gtk_box_append(GTK_BOX(box), image);
        }
        if (display != NULL) {
            text = display;
        }
    }
    label = gtk_label_new(text);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), LABEL_CHARS);
    gtk_widget_set_tooltip_text(label, text);
    gtk_box_append(GTK_BOX(box), label);
    gtk_button_set_child(GTK_BUTTON(button), box);
    g_signal_connect_data(button, "clicked", G_CALLBACK(open_entry), g_file_get_uri(file),
                          free_uri, 0);

    if (info != NULL) {
        g_object_unref(info);
    }
    g_object_unref(file);
    g_free(path);
    return button;
}

static void fill_icons(void) {
    const gchar *directory = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    GtkWidget *child;
    GList *names = NULL;
    GList *item;
    GDir *dir;
    const gchar *name;

    while ((child = gtk_widget_get_first_child(flow)) != NULL) {
        gtk_flow_box_remove(GTK_FLOW_BOX(flow), child);
    }
    if (directory == NULL) {
        return;
    }
    dir = g_dir_open(directory, 0, NULL);
    if (dir == NULL) {
        return;
    }
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] != '.') {
            names = g_list_prepend(names, g_strdup(name));
        }
    }
    g_dir_close(dir);
    names = g_list_sort(names, compare_names);
    for (item = names; item != NULL; item = (*item).next) {
        gtk_flow_box_append(GTK_FLOW_BOX(flow), build_entry(directory, (const gchar *)(*item).data));
    }
    g_list_free_full(names, g_free);
}

static gboolean run_refill(gpointer data) {
    (void)data;
    refill_source = 0;
    fill_icons();
    return G_SOURCE_REMOVE;
}

static void schedule_refill(void) {
    if (refill_source == 0) {
        refill_source = g_timeout_add(REFILL_DELAY_MS, run_refill, NULL);
    }
}

static void on_directory_changed(GFileMonitor *source, GFile *file, GFile *other,
                                 GFileMonitorEvent event, gpointer data) {
    (void)source;
    (void)file;
    (void)other;
    (void)data;
    switch (event) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        schedule_refill();
        break;
    default:
        break;
    }
}

static void watch_directory(void) {
    const gchar *directory = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    GFile *file;

    if (directory == NULL) {
        return;
    }
    file = g_file_new_for_path(directory);
    monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    g_object_unref(file);
    if (monitor != NULL) {
        g_signal_connect(monitor, "changed", G_CALLBACK(on_directory_changed), NULL);
    }
}

static GtkWidget *build_icons(void) {
    flow = gtk_flow_box_new();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(flow), GTK_ORIENTATION_VERTICAL);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), ROWS_PER_COLUMN);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_widget_set_halign(flow, GTK_ALIGN_START);
    gtk_widget_set_valign(flow, GTK_ALIGN_START);
    fill_icons();
    watch_directory();
    return flow;
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *overlay = gtk_overlay_new();

    (void)data;
    gtk_window_set_title(GTK_WINDOW(window), "desktop_de");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), build_icons());
    gtk_window_set_child(GTK_WINDOW(window), overlay);

    refresh_wallpaper(NULL);
    g_timeout_add_seconds(REFRESH_SECONDS, refresh_wallpaper, NULL);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("de.bare.desktop", G_APPLICATION_NON_UNIQUE);
    int status;

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_clear_object(&monitor);
    g_free(loaded_path);
    g_object_unref(app);
    return status;
}