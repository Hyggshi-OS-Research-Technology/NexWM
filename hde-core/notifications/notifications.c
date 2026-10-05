#include "hde/notifications.h"
#include <stdio.h>
#include <stdlib.h>
int hde_notify(const char *summary,const char *body,int timeout_ms){(void)timeout_ms; if(!summary)summary="HDE"; if(!body)body=""; char cmd[2048]; snprintf(cmd,sizeof(cmd),"notify-send -- '%s' '%s'",summary,body); return system(cmd);}
