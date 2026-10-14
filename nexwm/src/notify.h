/* notify.h — NexWM's small bridge to the desktop notification service. */
#ifndef NEXWM_NOTIFY_H
#define NEXWM_NOTIFY_H

void nexwm_notify_app_unresponsive(const char *title);
void nexwm_notify_app_unresponsive_ex(const char *title, unsigned long xid, int pid);

#endif
