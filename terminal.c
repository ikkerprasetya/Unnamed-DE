#include <gtk/gtk.h>
#include <vte/vte.h>

#define DEFAULT_TIMEOUT (~0)

static void on_child_exited(VteTerminal *terminal, gint status, gpointer data) {
    gtk_window_destroy(GTK_WINDOW(data));
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *terminal = vte_terminal_new();
    const gchar *shell = g_getenv("SHELL");
    gchar *argv[2];

    if (shell == NULL) {
        shell = "/bin/sh";
    }
    argv[0] = (gchar *)shell;
    argv[1] = NULL;

    gtk_window_set_title(GTK_WINDOW(window), "Terminal");
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 440);
    gtk_window_set_child(GTK_WINDOW(window), terminal);
    g_signal_connect(terminal, "child_exited", G_CALLBACK(on_child_exited), window);
    vte_terminal_spawn_async(VTE_TERMINAL(terminal), VTE_PTY_DEFAULT, g_get_home_dir(),
                             argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
                             DEFAULT_TIMEOUT, NULL, NULL, NULL);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("de.bare.terminal", G_APPLICATION_NON_UNIQUE);
    int status;

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
