#!/usr/bin/env python3
"""App SNI thật (libayatana-appindicator + libdbusmenu) để thử tray của hde-panel."""
import gi, sys
gi.require_version("Gtk", "3.0")
gi.require_version("AyatanaAppIndicator3", "0.1")
from gi.repository import Gtk, GLib, AyatanaAppIndicator3 as AI

ind = AI.Indicator.new("hde-test-app", "mail-unread", AI.IndicatorCategory.APPLICATION_STATUS)
ind.set_status(AI.IndicatorStatus.ACTIVE)
ind.set_title("HDE test indicator")

menu = Gtk.Menu()
def img_item(label, icon):
    mi = Gtk.ImageMenuItem.new_with_mnemonic(label)
    mi.set_image(Gtk.Image.new_from_icon_name(icon, Gtk.IconSize.MENU))
    mi.set_always_show_image(True)
    return mi
for label, icon in (("_Mở cửa sổ", "document-open"), ("_Cài đặt", "preferences-system"), ("_Thông tin", "help-about")):
    mi = img_item(label, icon)
    mi.connect("activate", lambda w, l=label: print("CLICKED", l, flush=True))
    menu.append(mi)
menu.append(Gtk.SeparatorMenuItem())
chk = Gtk.CheckMenuItem.new_with_label("Bật thông báo"); chk.set_active(True); menu.append(chk)
plain = Gtk.MenuItem.new_with_label("Mục không icon"); menu.append(plain)
quit_ = img_item("_Thoát", "application-exit")
quit_.connect("activate", lambda w: Gtk.main_quit()); menu.append(quit_)
menu.show_all()
ind.set_menu(menu)
print("READY", flush=True)
Gtk.main()
