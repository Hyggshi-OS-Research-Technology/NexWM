# hello-c — a NexSDK application template

A C program for Hyggshi OS that uses [NexSDK](../../README.md) and is packaged as a `.deb`.

```sh
cp -r /usr/share/nexsdk/templates/hello-c myapp
cd myapp
cmake -S . -B build && cmake --build build
./build/hello
```

## Packaging

```sh
debuild -us -uc                 # with debian/ below
hde-pkg-check ../hello-c_1.0-1_amd64.deb
```

`hde-pkg-check` is the last step before publishing: it looks at the control fields, the files and the
`.desktop` entry, and says what is missing (a Depends, an icon, a description that is more than one line).

## Where things go

| what | where |
| --- | --- |
| the program | `/usr/bin/hello` |
| the menu entry | `/usr/share/applications/hello.desktop` |
| the copyright | `/usr/share/doc/hello-c/copyright` (Debian policy requires it) |
