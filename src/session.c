#include <gtk/gtk.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

enum { ACTION_LOGOUT, ACTION_SUSPEND, ACTION_RESTART, ACTION_SHUTDOWN, ACTION_COUNT };

static const char *labels[ACTION_COUNT] = {"Logout", "Suspend", "Restart", "Shutdown"};
static const int needs_confirm[ACTION_COUNT] = {0, 0, 1, 1};

static GtkWidget *window;
static GtkWidget *prompt;
static GtkWidget *buttons[ACTION_COUNT];
static int pending = ACTION_COUNT;

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

static void reset_pending(void) {
    int i;

    for (i = 0; i < ACTION_COUNT; i++) {
        gtk_button_set_label(GTK_BUTTON(buttons[i]), labels[i]);
    }
    gtk_label_set_text(GTK_LABEL(prompt), "Choose an action");
    pending = ACTION_COUNT;
}

static void execute(int action) {
    const gchar *session_pid = g_getenv("DE_SESSION_PID");
    char *suspend[3] = {"systemctl", "suspend", NULL};
    char *reboot[3] = {"systemctl", "reboot", NULL};
    char *power_off[3] = {"systemctl", "poweroff", NULL};

    if (action == ACTION_LOGOUT) {
        if (session_pid != NULL) {
            kill(atoi(session_pid), SIGTERM);
        }
    } else if (action == ACTION_SUSPEND) {
        run(suspend);
    } else if (action == ACTION_RESTART) {
        run(reboot);
    } else if (action == ACTION_SHUTDOWN) {
        run(power_off);
    }
    gtk_window_destroy(GTK_WINDOW(window));
}

static void on_action(GtkButton *button, gpointer data) {
    int action = GPOINTER_TO_INT(data);
    gchar *text;

    if (needs_confirm[action] && pending != action) {
        reset_pending();
        pending = action;
        text = g_strdup_printf("Confirm %s", labels[action]);
        gtk_button_set_label(GTK_BUTTON(buttons[action]), text);
        g_free(text);
        gtk_label_set_text(GTK_LABEL(prompt), "Press again to confirm");
        return;
    }
    execute(action);
}

static void on_cancel(GtkButton *button, gpointer data) {
    if (pending != ACTION_COUNT) {
        reset_pending();
        return;
    }
    gtk_window_destroy(GTK_WINDOW(window));
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                       GdkModifierType state, gpointer data) {
    if (keyval == GDK_KEY_Escape) {
        on_cancel(NULL, NULL);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkEventController *keys = gtk_event_controller_key_new();
    int i;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Session");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 260, 200);

    prompt = gtk_label_new("");
    gtk_box_append(GTK_BOX(box), prompt);
    for (i = 0; i < ACTION_COUNT; i++) {
        buttons[i] = gtk_button_new_with_label(labels[i]);
        g_signal_connect(buttons[i], "clicked", G_CALLBACK(on_action), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(box), buttons[i]);
    }
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), NULL);
    gtk_box_append(GTK_BOX(box), cancel);

    g_signal_connect(keys, "key_pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(window, keys);

    reset_pending();
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