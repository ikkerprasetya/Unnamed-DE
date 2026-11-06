#include <gtk/gtk.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

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

static void on_logout(GtkButton *button, gpointer data) {
    const gchar *pid = g_getenv("DE_SESSION_PID");
    if (pid != NULL) {
        kill(atoi(pid), SIGTERM);
    }
}

static void on_restart(GtkButton *button, gpointer data) {
    char *arguments[3] = {"systemctl", "reboot", NULL};
    run(arguments);
}

static void on_power_off(GtkButton *button, gpointer data) {
    char *arguments[3] = {"systemctl", "poweroff", NULL};
    run(arguments);
}

static void on_cancel(GtkButton *button, gpointer data) {
    gtk_window_destroy(GTK_WINDOW(data));
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *window = gtk_application_window_new(app);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *logout = gtk_button_new_with_label("Logout");
    GtkWidget *restart = gtk_button_new_with_label("Restart");
    GtkWidget *power_off = gtk_button_new_with_label("Shutdown");
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");

    gtk_window_set_title(GTK_WINDOW(window), "Session");
    gtk_window_set_default_size(GTK_WINDOW(window), 240, 160);
    g_signal_connect(logout, "clicked", G_CALLBACK(on_logout), NULL);
    g_signal_connect(restart, "clicked", G_CALLBACK(on_restart), NULL);
    g_signal_connect(power_off, "clicked", G_CALLBACK(on_power_off), NULL);
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), window);
    gtk_box_append(GTK_BOX(box), logout);
    gtk_box_append(GTK_BOX(box), restart);
    gtk_box_append(GTK_BOX(box), power_off);
    gtk_box_append(GTK_BOX(box), cancel);
    gtk_window_set_child(GTK_WINDOW(window), box);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("de.bare.session", G_APPLICATION_NON_UNIQUE);
    int status;

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}