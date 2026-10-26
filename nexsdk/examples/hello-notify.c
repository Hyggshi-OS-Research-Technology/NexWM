/* nexsdk/examples/hello-notify.c — a notification that cannot be turned into a command.
 *
 * Try it with a nasty summary:
 *   ./hello-notify "It's done; rm -rf ~/ \$(whoami) `id`"
 * The whole thing arrives as text, because NexSDK never goes through a shell.
 */
#include <nexsdk/nexsdk.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    NexSdkNotification n = NEXSDK_NOTIFICATION_INIT;
    n.summary = argc > 1 ? argv[1] : "Hello from NexSDK";
    n.body = argc > 2 ? argv[2] : "This notification was sent without a toolkit and without a shell.";
    n.icon = "dialog-information";
    n.app_name = "NexSDK";
    n.timeout_ms = 5000;

    if (!nexsdk_notify_available()) {
        fprintf(stderr, "no notification service here (no session bus, or notify-send is not installed)\n");
        return 1;
    }
    int rc = nexsdk_notify_send(&n);
    if (rc != 0) fprintf(stderr, "notify-send said %d\n", rc);
    return rc;
}
