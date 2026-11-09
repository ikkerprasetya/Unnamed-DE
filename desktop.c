#include <gtk/gtk.h>

static GtkWidget *picture;
static gchar *loaded_path;

static gboolean refresh_wallpaper(gpointer data) {
    gchar *file = g_build_filename(g_get_user_config_dir(), "de", "wallpaper", NULL);
    gchar *contents = NULL;

    if (g_file_get_contents(file, &contents, NULL, NULL)) {
        g_strstrip(contents);
        if (g_strcmp0(contents, loaded_path) != 0) {
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
    g_app_info_launch_default_for_uri((const char *)data, NULL, NULL);
}

static GtkWidget *build_icons(void) {
    GtkWidget *flow = gtk_flow_box_new();
    const gchar *directory = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    GDir *dir;
    const gchar *name;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(flow), GTK_ORIENTATION_VERTICAL);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 10);
    gtk_widget_set_halign(flow, GTK_ALIGN_START);
    gtk_widget_set_valign(flow, GTK_ALIGN_START);
    if (directory == NULL) {
        return flow;
    }
    dir = g_dir_open(directory, 0, NULL);
    if (dir == NULL) {
        return flow;
    }
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *path;
        gchar *uri;
        GtkWidget *button;

        if (name[0] == '.') {
            continue;
        }
        path = g_build_filename(directory, name, NULL);
        uri = g_filename_to_uri(path, NULL, NULL);
        g_free(path);
        button = gtk_button_new_with_label(name);
        g_signal_connect(button, "clicked", G_CALLBACK(open_entry), uri);
        gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
    }
    g_dir_close(dir);
    return flow;
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *overlay = gtk_overlay_new();

    gtk_window_set_title(GTK_WINDOW(window), "desktop_de");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), build_icons());
    gtk_window_set_child(GTK_WINDOW(window), overlay);

    refresh_wallpaper(NULL);
    g_timeout_add_seconds(3, refresh_wallpaper, NULL);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("de.bare.desktop", G_APPLICATION_NON_UNIQUE);
    int status;

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
