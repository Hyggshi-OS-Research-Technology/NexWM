# The HDE login screen (SDDM)

The login screen of the Hyggshi Desktop Environment, in its own folder, with everything it needs in it:

```
login/sddm/
├── hde/                  the theme itself (copy this folder into /usr/share/sddm/themes/ to use it by hand)
│   ├── Main.qml          the screen: background, card, clock, users, password, session, layout, power
│   ├── theme.conf        what it shows and how (one file, commented: colours, texts, clock, user list, ...)
│   ├── theme.conf.user   (optional, you create it) only the lines you want to change; it survives updates
│   ├── metadata.desktop  what SDDM reads to find the theme (QtVersion=5 or 6, the greeter to run it in)
│   ├── components/       the widgets of the screen, each in its own file
│   │   ├── HdeBackground.qml   the picture (or colour) with the dimming that keeps white text readable
│   │   ├── HdeLogo.qml         the HDE "H" drawn with rectangles — no picture to ship
│   │   ├── HdeIcons.qml        the small signs (user, power, moon, lock, chevron, eye, tick) drawn the same way
│   │   ├── HdeClock.qml        time, date and the name of the computer
│   │   ├── HdeUsers.qml        the users of this computer as tiles (avatar + real name), with the tick on the chosen
│   │   ├── HdeField.qml        a text field (the user name, in `userMode=username`)
│   │   ├── HdePassword.qml     the password field: accent while it has the cursor, the eye to reveal, Caps Lock note
│   │   ├── HdeCombo.qml        a chooser of the screen (session, layout): field + list
│   │   ├── HdeSessions.qml     the sessions the greeter offers (HDE, HDE (Wayland), NexWM, ...)
│   │   ├── HdeLayouts.qml      the keyboard layouts (SDDM 0.19 and newer)
│   │   ├── HdeButton.qml       the Sign in button, with a spinner while SDDM checks the password
│   │   ├── HdeMessage.qml      "Wrong password…", and it shakes so it is noticed
│   │   ├── HdePower.qml        sleep / restart / shut down (only the ones this machine supports)
│   │   └── HdeIconButton.qml   the round buttons of the power row
│   └── assets/
│       ├── background.png      drawn by tools/make-background.py (2560x1440, 152 KiB, deterministic)
│       └── screenshot.png      the picture metadata.desktop points at (what the theme choosers show)
├── tools/
│   └── make-background.py      draws assets/background.png with no image library (python3 only)
└── install.sh                  installs the theme and makes SDDM use it (also installed as `hde-login`)
```

## Install it

```sh
sudo sh login/sddm/install.sh            # from the checkout
sudo hde-login                           # after "sudo make install" (the same script, installed as hde-login)
```

The script

* copies the theme to `/usr/share/sddm/themes/hde` (the `--dir` option puts it somewhere else),
* writes `/etc/sddm.conf.d/50-hde-theme.conf` with

  ```ini
  [Theme]
  Current=hde
  ```

  so SDDM uses it (Debian, Fedora, openSUSE and Arch all read `/etc/sddm.conf.d`, which wins over the distribution's
  own files in `/usr/lib/sddm/sddm.conf.d`),
* with `--default-session`: `[Users] DefaultSession=hde.desktop` (SDDM 0.20 and newer) and the `[Last] Session` line of
  `state.conf`, which is what older SDDM versions preselect,
* rewrites `metadata.desktop`'s `QtVersion` to the greeter this machine actually has (`sddm-greeter` → 5,
  `sddm-greeter-qt6` → 6). SDDM picks the greeter binary from that line, and the same theme has to work on Debian (Qt 5)
  and Fedora (Qt 6) — this is why it is written at install time, not hard-coded here,
* `--dry-run` says what it would do and changes nothing; `--uninstall` removes the theme and the configuration it wrote;
  `--root DIR` installs under `DIR` as if it were `/` (packaging, images, tests).

Then:

```sh
sudo systemctl restart sddm        # log out first! (the screen you are looking at is the one that restarts)
sddm-greeter --test --theme /usr/share/sddm/themes/hde     # or in a window, without logging out
```

## Change the look

Everything is in `hde/theme.conf`:

```ini
[General]
accent=#3584e4          # the line on the card, the focus, the buttons, the chosen user
background=assets/background.png   # a picture of this theme, a path of your own, or a colour like #0e1520
dim=0.30               # how much the picture is dimmed (0 = as it is, 1 = dark)
title=Hyggshi Desktop Environment
footer=HDE · Hyggshi OS
clockPosition=top-right   # top-left / top-center / top-right
userMode=user          # user: the list of the users of this computer, username: type a name
showUserList=true      # the users as tiles; with no list to show, a field to type a name appears instead
cardWidth=400
```

Who is logging in: `userMode=user` shows the users of this computer as tiles (the avatar and the real name), and
`userMode=username` a field to type a name in. The field is also what appears when there is no list to show —
`showUserList=false`, or a greeter that hands over no users at all: SDDM leaves accounts out of its list by uid range
(`[Users] MinimumUid` / `MaximumUid` in `/etc/sddm.conf.d`) and by `HideUsers` / `HideShells`, and without the field the
card would have no way to say who is logging in and the *Sign in* button would stay grey. The screen says so in the
greeter's log (`journalctl -u sddm`, or the log of the session that started the greeter):

    hde-login: the greeter listed 1 user(s)
    hde-login: the greeter listed no users: the login screen offers a user name field
    hde-login: the keyboard is in the password field        # where the typing goes (the name field without a list)

`hde-lock`-style diagnostics, for the login screen: if the log says *no users* and that is not what you expect, look at
`HideUsers`, `HideShells` and the uid range of your account (`getent passwd "$USER"`).

Rather than editing that file (an update overwrites it), put only your lines in `hde/theme.conf.user`: SDDM reads it
after `theme.conf` and what it contains wins.

A new background: `python3 login/sddm/tools/make-background.py 3840 2160 /tmp/bg.png` (colours and the horizon are at
the top of that file), or simply point `background=` at any picture.

## What it needs

* SDDM 0.20 or newer (`sddm` on Debian, Fedora, openSUSE and Arch). The theme works in the Qt 5 and the Qt 6 greeter —
  it needs nothing but core QtQuick: no QtGraphicalEffects (Qt 6 removed it), no QtQuick.Controls, and not even SDDM's
  own component module: every widget of the screen is drawn in `components/`, which is also why the login screen looks
  the same on every distribution.
* A font (any), and `python3` only if you want to redraw the background.

Optional parts it uses when SDDM provides them (and quietly skips when it does not):

* `keyboard.layouts` (SDDM 0.19+): the keyboard layout chooser appears only when there is more than one layout;
* `sddm.canSuspend` / `canReboot` / `canPowerOff`: the power buttons, one by one;
* the user model's `icon` role: the avatar of a user who set one (`~/.face`), otherwise the drawn placeholder;
* `sddm.hostName`: the name of the computer under the clock.

## Test it

```sh
sh tests/sddm-test.sh            # everything this machine can do
sh tests/sddm-test.sh --no-render   # files, metadata, QML, theme.conf and the installer (no display needed)
make check-login                 # the same
python3 tests/sddm-qml-test.py   # the theme in a real QML engine, with the greeter made in the test (needs PySide6)
```

The test checks the parts that are easy to get wrong: the files SDDM needs are there, `metadata.desktop` says what SDDM
reads, the QML balances and imports nothing a greeter may not have, every file the theme names exists, `theme.conf`
explains exactly the keys `Main.qml` reads, the installer writes (and `--uninstall` removes) the right files — and then,
if SDDM's greeter and an X display are available (the CI job installs both), it renders the theme in the greeter, reads
the greeter's log for QML errors and looks at the pixels of the screen: the dark background, the card, the accent line
on it — and whether typing reaches the card. The login screen of a distribution is not something that can be checked by
looking at it once.

The test also keeps pictures of the screen it looked at (`shot-sddm-*.png` in `$HDE_TEST_OUT`, uploaded as the
`hde-sddm-login` artifact of the CI job): the login screen as it really renders — with the user tiles, and with the
field to type a user name in when the greeter hands over no users. Nothing else shows a reader what their login screen
will look like without logging out of their own session.

`tests/sddm-qml-test.py` goes one step further and does what a machine's own greeter cannot be asked for: it loads
`Main.qml` with a greeter made for the occasion (PySide6, Qt 6, offscreen — no display, no SDDM, no root) and checks
three situations: one user from the greeter (tiles, the keyboard in the password field), an *empty* user list, and no
user model at all. In the last two, the card has to offer a field to type a user name in, the keyboard has to be in it,
and typing a name has to reach `sddm.login()`. Without PySide6 the test says so and skips (`pip install
PySide6-Essentials` to run it); the CI job installs it.

CI: the `SDDM login theme` job (Ubuntu, Qt 5 greeter, and the QML-engine test above) and the `Fedora (dnf, full
desktop)` job (Fedora, Qt 6 greeter) both run this test, so both greeters are covered.

## Credits

The QML here is original (see `hde/authors.md`); the layout of the screen follows what the login screens of the big
desktops do, and the greeter contract (metadata keys, context objects, models) comes from SDDM's own documentation and
source. No files of SDDM's themes are copied.
