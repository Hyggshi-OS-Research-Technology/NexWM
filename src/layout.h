/*
 * layout.h - Layout engine for NexWM
 */

#ifndef NEXWM_LAYOUT_H
#define NEXWM_LAYOUT_H

#include "client.h"
#include "monitor.h"
#include "config.h"

void nex_layout_tile(nex_monitor_t *m);
void nex_layout_floating(nex_monitor_t *m);
void nex_layout_monocle(nex_monitor_t *m);
void nex_layout_apply(nex_monitor_t *m, nex_layout_t layout);

#endif
