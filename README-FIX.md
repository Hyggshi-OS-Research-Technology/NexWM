# HDE session black-screen fix

The old `hde-session` initialized the backend, printed one line, shut the backend down and exited. That caused the Xephyr session to become a black/empty screen.

This version keeps the session alive, resolves `hde-desktop` and `hde-panel` relative to the `hde-session` executable (then PATH and standard bin directories), starts them, and cleans them up on SIGTERM/SIGINT.

## Test with the existing NexDE binaries

Build the migration tree:

    make

Then make sure your real NexDE build contains:

    build/hde-session
    build/hde-desktop
    build/hde-panel

Run:

    Xephyr :2 -screen 1280x720 &
    DISPLAY=:2 ./build/hde-session

If `hde-session` is installed beside the desktop/panel binaries, it will launch them automatically.

Optional:

    DISPLAY=:2 ./build/hde-session --no-panel
    DISPLAY=:2 ./build/hde-session --no-desktop

## Lỗi "icon / menu Start không bấm được" (bản sửa 2)

Nguyên nhân thật khi chạy `DISPLAY=:2 ./build/hde-session` trong Xephyr:

1. `resolve_component()` dùng *static buffer*: `hde-desktop` và `hde-panel` trỏ cùng một chuỗi ->
   session chạy **hai panel, không có desktop** (log: `starting desktop: .../hde-panel`).
2. Host chạy Wayland (`WAYLAND_DISPLAY` được set) nên backend chọn `wayland`, và GTK mở cửa sổ
   trên host thay vì trong Xephyr. Giờ: có `DISPLAY` thì dùng X11, session set `GDK_BACKEND=x11`.
3. Binary cũ trong `/usr/local/bin` (log tiếng Việt) được dùng thay vì bản mới. `make` giờ build
   `hde-desktop`, `hde-panel`, `hde-settings` từ `src/` vào `build/`; session tìm cạnh chính nó trước.
4. Session không chạy window manager -> thêm (xfwm4, openbox, ...); tắt bằng `--no-wm` hoặc `HDE_NO_WM=1`.

Chạy lại:

    make clean && make
    Xephyr :2 -screen 1280x720 &
    DISPLAY=:2 ./build/hde-session

## Chạy được trong Xephyr nhưng không lên trên máy thật (bản sửa 3)

Trên máy thật phiên được gọi qua `hde.desktop` -> `hde-start` -> `hde-session`, nhưng trước đây
`make install` chỉ cài `hde-session`: không cài `hde.desktop` (xsessions), không cài `hde-start`
(file cũng không có quyền chạy), và `@PREFIX@` không bao giờ được thay -> màn hình đăng nhập
không có phiên HDE hợp lệ, hoặc phiên thoát ngay.

Cài đặt:

    sudo apt install libgtk-3-dev libwnck-3-dev openbox   # openbox hoặc xfwm4: window manager
    make clean && make
    sudo make install            # PREFIX=/usr/local mặc định

Sau đó đăng xuất, ở màn hình đăng nhập chọn phiên **HDE** (biểu tượng bánh răng), phiên X11.
Nếu phiên không lên, xem: `~/.cache/hde/session.log` (và `~/.xsession-errors`).

Không có display manager: `echo 'exec /usr/local/bin/hde-start' > ~/.xinitrc && startx`

## Chuột / input không bấm được (bản sửa 4)

1. **Chạy lồng trong Xephyr trên host Wayland**: GTK3 dùng XInput2 nên không nhận click chuột thật
   từ Xephyr (xdotool/XTEST vẫn chạy nên khó nhận ra). `hde-session` và `hde-start` giờ tự đặt
   `GDK_CORE_DEVICE_EVENTS=1` khi có `WAYLAND_DISPLAY` hoặc `HDE_CORE_EVENTS=1`.
   Tắt bằng `HDE_XI2=1`. Thử thủ công: `GDK_CORE_DEVICE_EVENTS=1 DISPLAY=:2 ./build/hde-session`.
2. **Hộp Run**: Enter không chạy lệnh (thiếu default response) -> đã sửa, Enter = Run.
3. `hde-start` đặt con trỏ gốc `left_ptr` (nếu có `xsetroot`) để chuột luôn hiện trên phiên thật.

Chạy lại:

    make clean && make
    Xephyr :2 -screen 1280x720 &
    DISPLAY=:2 ./build/hde-session


## HDE real-machine additions

- `hde-hotkeys`: X11 system shortcut daemon.
  - `PrtSc/SysRq`: full-screen screenshot.
  - `Shift+PrtSc`: area screenshot.
  - `Alt+PrtSc`: active-window screenshot.
  - Uses `gnome-screenshot`, `xfce4-screenshooter`, `flameshot`, or `scrot` as available.
- `hde-session` starts the window manager before GTK desktop components.
  - `HDE_WM=auto` uses the fallback list.
  - `HDE_WM=xfwm4`, `openbox`, `marco`, etc. selects a specific WM.
- Panel status shows input method/Fcitx, Wi-Fi and Bluetooth state from the real machine.
- Settings adds Bluetooth and Window Management pages.
- `settings.ini` can contain `wm=xfwm4` (or `auto`) and `hde-start` passes it to the session.
