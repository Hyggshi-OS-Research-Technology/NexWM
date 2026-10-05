#include "hde/backend.h"
#include <stdlib.h>
#include <stdio.h>
HDEBackend *hde_backend_wayland_create(void){ HDEBackend*b=calloc(1,sizeof(*b)); if(!b)return NULL; b->type=HDE_BACKEND_WAYLAND; b->name="wayland"; return b; }
