/* player.h — the media player's window (the second part of hde-media; viewer.h is the first, the recorder is the
 * third). Same shape as viewer.h on purpose: main.c is allowed to know this and nothing more (hde-files/src/files.h
 * is the model). What plays the file is engine.c; the state of the playback (which track, what follows, volume) is
 * playlist.h — the window only shows it and starts the process.
 *
 * The window: what plays now at the top, the seek bar, the transport, the list of what was given, and a status line
 * with the engine and the repeat/shuffle state. Keys: Space play/pause, Left/Right (or b/n) previous and next,
 * s stop, Up/Down or +/- the volume, m mute, z shuffle, r repeat, q or Escape close.
 */
#ifndef HDE_MEDIA_PLAYER_H
#define HDE_MEDIA_PLAYER_H

#include <gtk/gtk.h>

typedef struct _HdeMediaPlayer HdeMediaPlayer;

/* a window playing `paths`: music and video files, folders (their music and video), and .m3u/.m3u8/.pls playlists
 * (with `recursive` a folder is walked all the way down). The first one starts playing at once. */
HdeMediaPlayer *hde_media_player_new(GtkApplication *app, char *const *paths, int n_paths, int recursive);
/* FALSE when there was nothing to play in what it was given: the caller says so and exits with status 2 */
int             hde_media_player_has_tracks(HdeMediaPlayer *p);
int             hde_media_player_alive(HdeMediaPlayer *p);        /* FALSE once the window was closed */
GtkWidget      *hde_media_player_widget(HdeMediaPlayer *p);
/* another hde-media (or a dropped file) while this window is running: added to the list, and the first of them plays */
void            hde_media_player_open(HdeMediaPlayer *p, char *const *paths, int n_paths, int recursive);
void            hde_media_player_present(HdeMediaPlayer *p);
void            hde_media_player_free(HdeMediaPlayer *p);         /* at shutdown */

#endif /* HDE_MEDIA_PLAYER_H */
