/* viewer.h — what main.c is allowed to know about the viewer window (model of hde-files/src/files.h: the window keeps
 * its state to itself). The media player and the recorder will sit next to it in this folder with the same shape:
 * hde_media_<thing>_new(), a way to ask what it shows, and a way to hand it new files while it runs.
 */
#ifndef HDE_MEDIA_VIEWER_H
#define HDE_MEDIA_VIEWER_H

#include <gtk/gtk.h>

#include "media.h"

typedef struct _HdeMediaViewer HdeMediaViewer;

/* a window showing `path` (a picture or a folder), or — with no path — the Pictures folder of this user. With more than
 * one path, the list is exactly those pictures in that order (what "Open With" passes). */
HdeMediaViewer *hde_media_viewer_new(GtkApplication *app, const char *path, char *const *paths, int n_paths,
                                     HdeMediaSort sort, int recursive, double interval, int slideshow, int fullscreen);
/* FALSE when there was nothing to show (no picture anywhere): the caller says so and exits with status 2 */
int             hde_media_viewer_has_pictures(HdeMediaViewer *v);
int             hde_media_viewer_alive(HdeMediaViewer *v);            /* FALSE once the window was closed */
GtkWidget      *hde_media_viewer_widget(HdeMediaViewer *v);
/* another hde-media (or a dropped picture) while this window is running */
void            hde_media_viewer_open(HdeMediaViewer *v, const char *path, char *const *paths, int n_paths);
void            hde_media_viewer_present(HdeMediaViewer *v);
void            hde_media_viewer_free(HdeMediaViewer *v);             /* at shutdown */

#endif /* HDE_MEDIA_VIEWER_H */
