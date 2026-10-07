# Hyggshi Files

The file manager of HDE (`hde-files`): GTK3, one process, no GVfs needed (it uses GVfs when it is there, for
`sftp://`, `smb://` and the like).

- **Views**: icons with thumbnails (pictures; videos, PDF and more when a thumbnailer is installed — shared with the
  other programs through `~/.cache/thumbnails`) or a list with Name, Size, Type and Modified (click a column to sort,
  folders stay first). Zoom with `Ctrl+plus` / `Ctrl+minus`, `Ctrl` + the mouse wheel or the slider of the status bar. Hidden files with `Ctrl+H`.
- **Getting around**: tabs (`Ctrl+T`, middle click on a folder), the places sidebar (home, bookmarks, drives, Trash,
  Recent; drop files on it), a path bar — `Ctrl+L` (or just type `/` or `~`) to type a folder, a file or an address,
  with completion —, back / forward / up (`Alt+Left`, `Alt+Right`, `Alt+Up`, `Backspace`, the mouse's side buttons),
  **search in the folder and its subfolders** (`Ctrl+F`), type the start of a name to jump to it.
- **Files**: copy / cut / paste (`Ctrl+C` / `Ctrl+X` / `Ctrl+V`, the same clipboard as the HDE desktop, Nautilus,
  Thunar…) and drag and drop (`Ctrl` copies, `Shift` moves, `Ctrl+Shift` links) in a worker thread with progress and
  Cancel in the status bar; when a name is taken: Replace, Skip, Keep Both or (folders) Merge, for one item or all of
  them; pasting into the same folder makes `name (copy).ext`. Rename (`F2`, or from the menu: the name is selected, not the extension — in HDE `F2` lowers the volume
  while the F1–F3 sound keys are on, *Settings → Keyboard & Shortcuts*),
  new folder (`Ctrl+Shift+N`), new document (empty or from `~/Templates`), make a link, compress (`.zip`, `.tar.gz`) and
  extract (zip, tar.*, 7z, rar with the usual programs), open a terminal there.
- **The Trash** (`Delete`): `trash:///` lists `~/.local/share/Trash` with where each item came from and when it was
  deleted; Restore puts it back where it was, Delete Permanently, Empty Trash. `Shift+Delete` deletes for good (asked
  first). **Undo** (`Ctrl+Z`): copy, move, move to the trash, rename, new folder / document.
- **Opening**: double click or `Enter` (`Ctrl+Enter` in a new tab, `Shift+Enter` in a new window); the right-click menu
  (or the Menu key) has *Open With* the default app, *Open With Other Application…* (and *Always use it*), and for
  programs and scripts *Run* / *Run in Terminal* / *Display*; launchers (`.desktop`) start their app.
- **Properties** (`Alt+Enter`): type, location, size (of everything inside a folder), dates, picture size, free space,
  the app that opens this type of file, and the permissions (owner / group / others, executable).
- Folders are watched: what other programs change shows at once. Light / Dark mode and the accent colour follow HDE's
  Settings live.

## Building

The top-level `make` builds it with the rest of HDE (`build/hde-files`) and `sudo make install` installs it. Alone:

```sh
cd hde-files
make                 # ./hde-files (needs libgtk-3-dev)
sudo make install    # /usr/local/bin/hde-files, its menu entry, hde-mimeapps.list
```

`hde-mimeapps.list` makes folders open in Hyggshi Files in HDE sessions (`XDG_CURRENT_DESKTOP=HDE`): the desktop, the
places of the Start menu. A default chosen by the user (`~/.config/mimeapps.list`, *Open With → Always use it*) comes
first. `Super+E` starts it, and the Screenshot tool's *Show in Folder* uses `hde-files --select`.

## Command line

```
hde-files [FOLDER|FILE|URI...]   windows / tabs on these folders (a file: its folder with the file selected)
hde-files --select PATH...       the folders of these items, with the items selected
hde-files --new-window           a new window even when one shows the folder already
hde-files --quit                 close the windows (file operations still running finish first)
```

Running it again opens a window in the running one (GtkApplication `org.hyggshi.Files`). While it runs it also owns
`org.freedesktop.FileManager1` (`ShowFolders`, `ShowItems`, `ShowItemProperties`): *Show in Folder* of web browsers and
download managers. `HDE_DEBUG=1` logs what it does on stderr (the smoke test reads it).

## Settings

`~/.config/hde/files.ini`, group `[files]`: `view` (`icons` / `list`), `icon_size` (32, 48, 64, 96, 128), `show_hidden`,
`sort` (`name`, `size`, `type`, `modified`), `sort_desc`, `folders_first`, `sidebar`, `sidebar_width`, `width`, `height`,
`maximized`, `single_click`, `thumbnails`. Bookmarks: `~/.config/gtk-3.0/bookmarks` (shared with the GTK file chooser).

## Sources (`src/`)

`files.h` (the parts and their functions), `main.c` (application, command line, FileManager1), `window.c` (a window:
tool bar, path bar, sidebar, tabs, status bar, actions and shortcuts, menus), `pane.c` (a tab: loading, watching,
views, sorting, search, Trash and Recent lists, thumbnails, drag and drop), `ops.c` (file operations, undo, compress /
extract), `trash.c` (the freedesktop.org trash), `thumbs.c` (thumbnails), `clipboard.c`, `dialogs.c` (rename, new
folder / document, properties, open with, run, conflicts, about, shortcuts), `util.c` (settings, formatting, icons,
menus, bookmarks, terminal). It uses `../src/hde-theme.c` for HDE's Light / Dark mode and accent colour.
