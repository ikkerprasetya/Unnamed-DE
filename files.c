#include <gtk/gtk.h>

static gchar *current_path;
static GtkWidget *list_box;
static GtkWidget *path_label;

static void refresh(void);

static void free_data(gpointer data, GClosure *closure) {
    g_free(data);
}

static gint compare_names(gconstpointer first, gconstpointer second) {
    return g_ascii_strcasecmp((const gchar *)first, (const gchar *)second);
}

static void open_entry(GtkButton *button, gpointer data) {
    gchar *path = g_strdup((const gchar *)data);
    gchar *uri;

    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(current_path);
        current_path = path;
        refresh();
        return;
    }
    uri = g_filename_to_uri(path, NULL, NULL);
    if (uri != NULL) {
        g_app_info_launch_default_for_uri(uri, NULL, NULL);
        g_free(uri);
    }
    g_free(path);
}

static void go_up(GtkButton *button, gpointer data) {
    gchar *parent = g_path_get_dirname(current_path);
    g_free(current_path);
    current_path = parent;
    refresh();
}

static void go_home(GtkButton *button, gpointer data) {
    g_free(current_path);
    current_path = g_strdup(g_get_home_dir());
    refresh();
}

static void refresh(void) {
    GtkWidget *child;
    GDir *dir;
    GList *names = NULL;
    const gchar *name;
    guint total;
    guint i;

    while ((child = gtk_widget_get_first_child(list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(list_box), child);
    }
    gtk_label_set_text(GTK_LABEL(path_label), current_path);
    dir = g_dir_open(current_path, 0, NULL);
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
    total = g_list_length(names);
    for (i = 0; i < total; i++) {
        const gchar *entry = g_list_nth_data(names, i);
        gchar *full = g_build_filename(current_path, entry, NULL);
        gchar *text;
        GtkWidget *button;

        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            text = g_strdup_printf("%s/", entry);
        } else {
            text = g_strdup(entry);
        }
        button = gtk_button_new_with_label(text);
        g_free(text);
        gtk_widget_set_halign(button, GTK_ALIGN_START);
        g_signal_connect_data(button, "clicked", G_CALLBACK(open_entry), full, free_data, 0);
        gtk_box_append(GTK_BOX(list_box), button);
    }
    g_list_free_full(names, g_free);
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *up_button = gtk_button_new_with_label("Up");
    GtkWidget *home_button = gtk_button_new_with_label("Home");
    GtkWidget *scroll = gtk_scrolled_window_new();

    gtk_window_set_title(GTK_WINDOW(window), "Files");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 520);

    path_label = gtk_label_new("");
    gtk_widget_set_hexpand(path_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(path_label), 0);
    list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    g_signal_connect(up_button, "clicked", G_CALLBACK(go_up), NULL);
    g_signal_connect(home_button, "clicked", G_CALLBACK(go_home), NULL);

    gtk_box_append(GTK_BOX(top), up_button);
    gtk_box_append(GTK_BOX(top), home_button);
    gtk_box_append(GTK_BOX(top), path_label);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_box);
    gtk_box_append(GTK_BOX(main_box), top);
    gtk_box_append(GTK_BOX(main_box), scroll);
    gtk_window_set_child(GTK_WINDOW(window), main_box);

    current_path = g_strdup(g_get_home_dir());
    refresh();
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("de.bare.files", G_APPLICATION_NON_UNIQUE);
    int status;

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
