/*
 * rules.c - Window rules implementation for NexWM
 */

#include "rules.h"
#include "config.h"
#include "log.h"
#include <string.h>

void nex_rules_apply(nex_client_t *c)
{
    if (!c) return;

    for (int i = 0; i < g_config.rule_count; i++) {
        nex_rule_t *r = &g_config.rules[i];
        int match = 1;

        if (r->class[0] && strcasecmp(r->class, c->class) != 0) match = 0;
        if (r->instance[0] && strcasecmp(r->instance, c->instance) != 0) match = 0;
        if (r->title[0] && strstr(c->title, r->title) == NULL) match = 0;

        if (match) {
            NEX_INFO("Applying rule to client 0x%x (class=\"%s\")", c->window, c->class);
            if (r->workspace >= 0) c->workspace = r->workspace;
            if (r->floating) c->flags |= NEX_CLIENT_FLOATING;
            if (r->fullscreen) c->flags |= NEX_CLIENT_FULLSCREEN;
        }
    }
}
