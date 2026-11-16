#include <gio/gio.h>
#include <gtk/gtk.h>

static const gchar introspection_xml[] =
    "<node>"
    "<interface name='org.freedesktop.Notifications'>"
    "<method name='GetCapabilities'>"
    "<arg type='as' direction='out'/>"
    "</method>"
    "<method name='Notify'>"
    "<arg type='s' name='app_name' direction='in'/>"
    "<arg type='u' name='replaces_id' direction='in'/>"
    "<arg type='s' name='app_icon' direction='in'/>"
    "<arg type='s' name='summary' direction='in'/>"
    "<arg type='s' name='body' direction='in'/>"
    "<arg type='as' name='actions' direction='in'/>"
    "<arg type='a{sv}' name='hints' direction='in'/>"
    "<arg type='i' name='expire_timeout' direction='in'/>"
    "<arg type='u' name='id' direction='out'/>"
    "</method>"
    "<method name='CloseNotification'>"
    "<arg type='u' name='id' direction='in'/>"
    "</method>"
    "<method name='GetServerInformation'>"
    "<arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/>"
    "</method>"
    "</interface>"
    "</node>";

static GDBusNodeInfo *node;
static GtkWidget *window;
static GtkWidget *summary_label;
static GtkWidget *body_label;
static guint timer;
static guint next_id = 1;

static gboolean hide_notification(gpointer data) {
    timer = 0;
    gtk_widget_set_visible(window, FALSE);
    return G_SOURCE_REMOVE;
}

static void show_notification(const gchar *summary, const gchar *body, guint seconds) {
    gtk_label_set_text(GTK_LABEL(summary_label), summary);
    gtk_label_set_text(GTK_LABEL(body_label), body);
    gtk_window_present(GTK_WINDOW(window));
    if (timer != 0) {
        g_source_remove(timer);
    }
    timer = g_timeout_add_seconds(seconds, hide_notification, NULL);
}

static void handle_method(GDBusConnection *connection, const gchar *sender,
                          const gchar *object_path, const gchar *interface_name,
                          const gchar *method_name, GVariant *parameters,
                          GDBusMethodInvocation *invocation, gpointer data) {
    if (g_strcmp0(method_name, "Notify") == 0) {
        const gchar *app;
        const gchar *icon;
        const gchar *summary;
        const gchar *body;
        guint replaces;
        guint id;
        guint seconds = 5;
        gint timeout;
        GVariant *actions;
        GVariant *hints;

        g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon,
                      &summary, &body, &actions, &hints, &timeout);
        if (timeout > 0) {
            seconds = (guint)(timeout / 1000) + 1;
        }
        id = replaces != 0 ? replaces : next_id++;
        show_notification(summary, body, seconds);
        g_variant_unref(actions);
        g_variant_unref(hints);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
    } else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&builder, "s", "body");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
    } else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(ssss)", "de", "bare", "1", "1.2"));
    } else if (g_strcmp0(method_name, "CloseNotification") == 0) {
        hide_notification(NULL);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable vtable = {handle_method, NULL, NULL};

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer data) {
    g_dbus_connection_register_object(connection, "/org/freedesktop/Notifications",
                                      g_dbus_node_info_lookup_interface(
                                          node, "org.freedesktop.Notifications"),
                                      &vtable, NULL, NULL, NULL);
}

static void activate(GtkApplication *app, gpointer data) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "notify_de");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 320, 90);

    summary_label = gtk_label_new("");
    body_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(summary_label), 0);
    gtk_label_set_xalign(GTK_LABEL(body_label), 0);
    gtk_label_set_wrap(GTK_LABEL(body_label), TRUE);
    gtk_box_append(GTK_BOX(box), summary_label);
    gtk_box_append(GTK_BOX(box), body_label);
    gtk_window_set_child(GTK_WINDOW(window), box);
    g_application_hold(G_APPLICATION(app));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("de.bare.notify", G_APPLICATION_NON_UNIQUE);
    int status;

    node = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
                   G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL, NULL, NULL, NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
