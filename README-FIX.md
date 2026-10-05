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

## Wi-Fi, Bluetooth, phím Super, F1–F3, WM GTK, Dark mode và các thành phần bắt buộc của DE (bản sửa 5)

### Đã sửa / thêm

| Yêu cầu | Trước | Bây giờ |
|---------|-------|---------|
| **Danh sách Wi-Fi** | Trang Network chỉ có dòng `nmcli device status` + công tắc Wi-Fi | Danh sách mạng thật (gộp theo SSID, sóng, bảo mật, đang kết nối / đã lưu), **Scan**, kết nối (hỏi mật khẩu — mật khẩu đưa vào `nmcli --ask` qua stdin, không lộ trong `ps`), ngắt kết nối, quên mạng, mạng ẩn, mạng doanh nghiệp (802.1X) mở trình chỉnh sửa nâng cao |
| **Bluetooth không hiện danh sách** | Chỉ "adapter detected" + công tắc | Đọc thẳng BlueZ qua D-Bus: **My devices** (đã ghép đôi: kết nối/ngắt/xoá, pin) + **Other devices** (quét 45 giây, ghép đôi có agent hỏi PIN/passkey/xác nhận mã), bật/tắt, hiện với thiết bị khác, tự `rfkill unblock`, báo rõ khi bluetoothd không chạy |
| **Super mở Start menu** | Không có | Nhấn rồi thả **Super** = mở/đóng menu (XInput2 raw events, không chiếm phím nên `Super+phím` của WM/app vẫn chạy). Đang mở menu gõ chữ = tìm ứng dụng |
| **F1, F2, F3 = âm thanh** | Không có | F1 tắt/bật tiếng, F2 giảm, F3 tăng (tối đa 100%), có OSD; phím media/độ sáng cũng chạy. Tắt F1–F3 trong *Settings → Keyboard & Shortcuts* để trả phím cho ứng dụng |
| **WM GTK thay cho Openbox/xfwm** | Danh sách xfwm4 → openbox → … | Hỗ trợ **Metacity, Marco, Mutter, Muffin** (ưu tiên ở chế độ Auto — viền cửa sổ theo theme GTK và Dark mode), đổi WM **ngay không cần đăng xuất** (*Settings → Window Management → Apply now* hoặc `hde-session wm`) |
| **Dark mode trong Settings cho đúng** | Combo "Dark" chỉ lưu số, không làm gì | Áp dụng ngay cho panel, menu, Settings và **mọi ứng dụng GTK đang mở** (trình nền `hde-xsettings`), ghi `~/.config/gtk-3.0/settings.ini`, GSettings `color-scheme` (GTK4/libadwaita), tự tìm biến thể tối của theme (tạo Adwaita-dark nếu thiếu `gnome-themes-extra`). Từ dòng lệnh: `hde-settings --style dark` |

### Những thứ "DE bắt buộc phải có" đã bổ sung

- **Trình nền thông báo** (`org.freedesktop.Notifications`) trong panel: popup, nút hành động, ảnh, âm báo, lịch sử ở nút chuông, **Do Not Disturb**.
- **Polkit agent**: hde-session tự chạy (polkit-gnome / mate / lxpolkit / kde…) để ứng dụng cần quyền admin hỏi được mật khẩu.
- **XDG autostart**: chạy các mục trong `~/.config/autostart` và `/etc/xdg/autostart` (tôn trọng `OnlyShowIn`/`NotShowIn`/`Hidden`/`TryExec`).
- **Tự chạy lại khi crash** (panel, desktop, hotkeys, xsettings, WM) có giới hạn số lần.
- **OSD** âm lượng/độ sáng, **lịch** khi bấm đồng hồ, ô **tìm ứng dụng**, khoá màn hình dùng trình khoá thật (light-locker, xscreensaver, dm-tool, i3lock…).
- `dbus-update-activation-environment` để các dịch vụ D-Bus (portal, keyring…) biết DISPLAY của phiên.
- Settings áp dụng thật: âm lượng/mic/thiết bị ra, bố cục bàn phím + tốc độ lặp phím, touchpad (xinput), tắt màn hình, cỡ chữ, hình nền (trước đây Settings lưu hình nền sai file nên desktop không đổi).
- Sửa build: `hde-core/include/hde/core.h` và `hde-core/integration/core.c` bị `.gitignore` (`core.*`) che nên repo không build được.

### Cài và thử

    sudo apt install build-essential pkg-config libgtk-3-dev libwnck-3-dev libxi-dev
    sudo apt install metacity network-manager bluez pipewire-pulse policykit-1-gnome libnotify-bin
    make clean && make
    sudo make install          # rồi đăng xuất, chọn phiên HDE

Thử nhanh không cài: `Xephyr :2 -screen 1280x720 & DISPLAY=:2 ./build/hde-session`.
Kiểm thử tự động (Xvfb): `sudo apt install xvfb xdotool dbus-x11 && make check`.

Lưu ý: chạy lồng trong Xephyr trên host Wayland, X server lồng có thể không phát XInput2 raw events,
khi đó phím Super không mở menu (bấm nút Menu hoặc `hde-panel --menu`); trên phiên thật thì bình thường.
