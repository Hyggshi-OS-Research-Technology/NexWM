/*
 * events.h - Event handling for NexWM
 */

#ifndef NEXWM_EVENTS_H
#define NEXWM_EVENTS_H

#include <xcb/xcb.h>

void nex_events_handle(xcb_generic_event_t *ev);

#endif
