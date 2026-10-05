#include "hde/backend.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
static int run(const char *cmd){ if(!cmd||!*cmd)return -1; pid_t p=fork(); if(p<0)return -1; if(p==0){setsid(); execl("/bin/sh","sh","-c",cmd,(char*)0); _exit(127);} return 0; }
static int init(HDEBackend*b){(void)b; return getenv("DISPLAY")?0:-1;} static void fini(HDEBackend*b){(void)b;}
static int launch(HDEBackend*b,const char*c){(void)b;return run(c);} static int logout(HDEBackend*b){(void)b;return run("loginctl terminate-user \"$USER\"");} static int reboot_(HDEBackend*b){(void)b;return run("systemctl reboot");} static int poweroff(HDEBackend*b){(void)b;return run("systemctl poweroff");} static int suspend_(HDEBackend*b){(void)b;return run("systemctl suspend");} static int lock_(HDEBackend*b){(void)b;return run("loginctl lock-session || xdg-screensaver lock");}
HDEBackend *hde_backend_x11_create(void){HDEBackend*b=calloc(1,sizeof(*b));if(!b)return NULL;b->type=HDE_BACKEND_X11;b->name="x11";b->init=init;b->shutdown=fini;b->launch=launch;b->logout=logout;b->reboot=reboot_;b->poweroff=poweroff;b->suspend=suspend_;b->lock=lock_;return b;}
