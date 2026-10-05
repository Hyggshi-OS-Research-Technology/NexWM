#ifndef HDE_SESSION_H
#define HDE_SESSION_H
int hde_session_init(void);
int hde_session_logout(void);
int hde_session_reboot(void);
int hde_session_shutdown(void);
int hde_session_suspend(void);
int hde_session_lock(void);
void hde_session_shutdown_core(void);
#endif
