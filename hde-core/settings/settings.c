#include "hde/settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static char path[1024];
/* Cùng vị trí với g_get_user_config_dir() của các thành phần GTK: $XDG_CONFIG_HOME hoặc ~/.config */
static void init_path(void){const char *x=getenv("XDG_CONFIG_HOME"); const char *h=getenv("HOME"); if(!h)h="/tmp"; char d[1024]; if(x&&*x) snprintf(d,sizeof(d),"%s/hde",x); else snprintf(d,sizeof(d),"%s/.config/hde",h); snprintf(path,sizeof(path),"%s/settings.ini",d); mkdir(d,0755);}
int hde_settings_init(void){init_path();return 0;}
int hde_settings_set(const char *key,const char *value){if(!path[0])init_path(); FILE *f=fopen(path,"a"); if(!f)return -1; fprintf(f,"%s=%s\n",key,value); fclose(f); return 0;}
const char *hde_settings_get(const char *key,const char *fallback){static char value[512]; FILE *f; char line[1024]; size_t n;if(!path[0])init_path(); f=fopen(path,"r"); if(!f)return fallback; n=strlen(key); while(fgets(line,sizeof(line),f)){if(!strncmp(line,key,n)&&line[n]=='='){snprintf(value,sizeof(value),"%s",line+n+1); value[strcspn(value,"\r\n")]=0; fclose(f); return value;}} fclose(f); return fallback;}
void hde_settings_shutdown(void){}
