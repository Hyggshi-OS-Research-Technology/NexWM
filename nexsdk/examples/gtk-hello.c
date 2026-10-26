/* nexsdk/examples/gtk-hello.c — a GTK window that knows the system it runs on.
 *
 * NexSDK has no toolkit in it, which is the point: the same three calls work in this GTK program, in a
 * command line tool and in a program that has a toolkit nobody has thought of yet.
 *
 *   cc $(pkg-config --cflags nexsdk gtk+-3.0) gtk-hello.c $(pkg-config --libs nexsdk gtk+-3.0) -o gtk-hello
 */
#include <nexsdk/nexsdk.h>
#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>

static void on_hello(GtkButton *b, gpointer data)
{
    (void)b;
    GtkLabel *label = GTK_LABEL(data);
    NexSdkNotification n = NEXSDK_NOTIFICATION_INIT;
    n.summary = "Hello from GTK";
    n.body = "A GTK program and NexSDK, with no code in between.";
    n.icon = "dialog-information";
    n.app_name = "NexSDK example";
    if (nexsdk_notify_send(&n) != 0)
        gtk_label_set_text(label, "There is no notification service in this session.");
    else
        gtk_label_set_text(label, "Notification sent.");
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    NexSdkSystem sys;
    nexsdk_system_read(&sys);
    char *summary = nexsdk_system_summary(&sys);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "NexSDK example");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 220);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_container_add(GTK_CONTAINER(win), box);

    GtkWidget *title = gtk_label_new(summary ? summary : "This machine");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    free(summary);

    char hde[256];
    snprintf(hde, sizeof hde, "HDE %s · session %s · NexSDK %s",
             sys.hde_version[0] ? sys.hde_version : "not installed", nexsdk_session_name(sys.session),
             nexsdk_version());
    GtkWidget *sub = gtk_label_new(hde);
    gtk_widget_set_halign(sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), sub, FALSE, FALSE, 0);

    GtkWidget *status = gtk_label_new("");
    GtkWidget *button = gtk_button_new_with_label("Tell me with a notification");
    g_signal_connect(button, "clicked", G_CALLBACK(on_hello), status);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
