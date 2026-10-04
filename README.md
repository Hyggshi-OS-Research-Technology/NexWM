# HDE – Hyggshi Desktop Environment (C + GTK3)

Thành phần: `hde-session` (quản lý phiên, tự restart khi crash, autostart), `hde-panel` (menu ứng dụng theo nhóm, taskbar, workspace pager, đồng hồ, đăng xuất/tắt máy), `hde-desktop` (hình nền, icon từ ~/Desktop, chuột phải: terminal/đổi nền/làm mới).

## Cài đặt
Debian/Ubuntu: `sudo apt install build-essential pkg-config libgtk-3-dev libwnck-3-dev librsvg2-common openbox dbus-x11`
Arch: `sudo pacman -S base-devel gtk3 libwnck3 openbox`

    make && sudo make install

Đăng xuất, chọn phiên **HDE** ở màn hình đăng nhập. Window manager tự dò: xfwm4, openbox, marco, metacity, icewm, fluxbox, nexwm.

## Thử nhanh không cần logout
    Xephyr :2 -screen 1280x720 &
    DISPLAY=:2 ./build/hde-session

## System tray
`hde-panel` có tray hỗ trợ XEmbed (GtkStatusIcon, app cũ) và StatusNotifierItem + dbusmenu (Electron, Telegram, nm-applet, ...).
Click trái = Activate, giữa = SecondaryActivate, phải = menu. Cần D-Bus session (script `hde-start` đã lo việc này).

- **Chưa có watcher nào:** hde-panel tự làm `org.kde.StatusNotifierWatcher`.
- **Đã có watcher khác** (xfce4-panel, Plasma, ...): hde-panel chuyển sang chế độ client — đăng ký làm host
  (`org.kde.StatusNotifierHost-<pid>`), đọc danh sách item hiện có và theo dõi `StatusNotifierItemRegistered/Unregistered`.
  Nếu watcher kia thoát, hde-panel tự lên thay (GDBus xếp hàng chờ tên) và app đăng ký lại.
- **Menu dbusmenu** hiển thị icon cạnh từng mục (`icon-name`, kể cả dự phòng bản `-symbolic`, và `icon-data` PNG).
  Cần `librsvg2-common` để icon symbolic (SVG) hiển thị được.

### Đã thử / chưa thử
Chạy `./scripts/test-tray` (server) và `./scripts/test-tray xfce` (cần xfce4-panel) — dùng Xvfb + D-Bus riêng.
Đã thử với: app libayatana-appindicator + libdbusmenu thật (`scripts/sni-test-app.py`), `nm-applet --indicator`
(đăng ký được, nhưng tự ẩn vì không có NetworkManager), và xfce4-panel thật làm watcher có sẵn.
**Chưa thử với Telegram** (Electron/Qt tự cài đặt SNI + dbusmenu riêng) — hãy thử trên máy bạn và báo lỗi nếu có.
