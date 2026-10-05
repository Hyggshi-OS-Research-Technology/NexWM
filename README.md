# HDE architecture migration

This tree establishes the HDE Core / Backend split without deleting the existing NexDE implementation.

## Layers
- `legacy/`: frozen Nex/X11/Openbox/Xfwm4 code kept for rollback and reference.
- `hde-core/`: backend-neutral APIs and core services.
- `backend/x11/`: X11 implementation.
- `backend/wayland/`: Wayland placeholder backend for the next phase.
- `apps/`: thin entry points.

## Build
```sh
make
./build/hde-core-demo
./build/hde-session
```

## Migration rule
New features must target `hde-core` APIs first. X11/Wayland-specific operations belong in `backend/*`. Legacy code is not deleted until the corresponding HDE implementation is verified.

## Next migration
1. Move desktop icon model and wallpaper logic from `legacy/x11/nex-desktop.c` into `hde-core/desktop`.
2. Move panel clock/start/power/taskbar model into `hde-core/panel`.
3. Move power/session actions behind `hde/session.h`.
4. Replace direct `nexwmctl` calls in settings with HDE settings + backend IPC.
5. Implement the Wayland backend after the X11 backend is feature-complete.
