/*
 * focus.h - Focus management for NexWM
 */

#ifndef NEXWM_FOCUS_H
#define NEXWM_FOCUS_H

#include "client.h"

void nex_focus_next(void);
void nex_focus_prev(void);
void nex_focus_direction(int dir);
void nex_focus_follow_mouse(xcb_window_t window);

#endif
