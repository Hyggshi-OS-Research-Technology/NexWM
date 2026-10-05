#include "hde/session.h"
#include "hde/core.h"
static int dispatch(int op){
    HDEBackend *b=hde_core_backend(); if(!b) return -1;
    switch(op){case 0:return b->logout?b->logout(b):-1;case 1:return b->reboot?b->reboot(b):-1;case 2:return b->poweroff?b->poweroff(b):-1;case 3:return b->suspend?b->suspend(b):-1;case 4:return b->lock?b->lock(b):-1;} return -1;
}
int hde_session_init(void){return hde_core_init(HDE_BACKEND_AUTO);}
int hde_session_logout(void){return dispatch(0);} int hde_session_reboot(void){return dispatch(1);} int hde_session_shutdown(void){return dispatch(2);} int hde_session_suspend(void){return dispatch(3);} int hde_session_lock(void){return dispatch(4);} void hde_session_shutdown_core(void){hde_core_shutdown();}
