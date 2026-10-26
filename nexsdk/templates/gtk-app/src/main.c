/* hello-gtk — a window, and the two things every HDE application ends up needing: what the system is,
 * and a way to tell the user something. */
#include <nexsdk/nexsdk.h>

#include <gtk/gtk.h>
#include <stdio.h>

static void on_notify(GtkButton *b, gpointer data)
{
    (void)b;
    GtkLabel *label = GTK_LABEL(data);
    NexSdkNotification n = NEXSDK_NOTIFICATION_INIT;
    n.summary = "Hello from GTK";
    n.body = "Sent by an application built with the Hyggshi OS template.";
    n.icon = "hello-gtk";
    n.app_name = "Hello GTK";
    if (nexsdk_notify_send(&n) == 0)
        gtk_label_set_text(label, "Notification sent.");
    else
        gtk_label_set_text(label, "No notification service in this session.");
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    NexSdkSystem sys;
    nexsdk_system_read(&sys);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Hello GTK");
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 260);
    gtk_window_set_icon_name(GTK_WINDOW(win), "hello-gtk");
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Hello GTK");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), sys.os_pretty);
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);
    gtk_container_add(GTK_CONTAINER(win), box);

    char *summary = nexsdk_system_summary(&sys);
    GtkWidget *label = gtk_label_new(summary ? summary : "This machine");
    free(summary);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget *status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    GtkWidget *button = gtk_button_new_with_label("Send a notification");
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "suggested-action");
    g_signal_connect(button, "clicked", G_CALLBACK(on_notify), status);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
