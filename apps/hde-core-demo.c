#include "hde/core.h"
#include <stdio.h>
int main(void){if(hde_core_init(HDE_BACKEND_AUTO)){fprintf(stderr,"HDE backend init failed\n");return 1;} HDEBackend*b=hde_core_backend();printf("HDE Core backend: %s\n",b?b->name:"none");hde_core_shutdown();return 0;}
