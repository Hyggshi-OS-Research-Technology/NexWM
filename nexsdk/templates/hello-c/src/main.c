/* hello — the NexSDK template: a program that uses the SDK and does one useful thing. */
#include <nexsdk/nexsdk.h>

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    NexSdkSystem sys;
    nexsdk_system_read(&sys);

    char *summary = nexsdk_system_summary(&sys);
    printf("Hello from Hyggshi OS\n%s\n", summary ? summary : "");
    free(summary);

    if (nexsdk_in_hde_session()) {
        NexSdkNotification n = NEXSDK_NOTIFICATION_INIT;
        n.summary = "Hello";
        n.body = "This program runs inside HDE and can notify you.";
        n.icon = "utilities-terminal";
        n.app_name = "Hello";
        nexsdk_notify_send(&n);
    }
    return 0;
}
