# library — the NexSDK library template

A shared library for Hyggshi OS: `include/mylib.h`, `src/mylib.c`, a `pkg-config` file and two packages
(`libmylib1` and `libmylib-dev`), because that is how Debian expects a library to be split.

```sh
cmake -S . -B build && cmake --build build
debuild -us -uc
hde-pkg-check ../*.deb
```

An application then uses it in the usual way:

```c
#include <mylib.h>
/* cc $(pkg-config --cflags --libs mylib) app.c */
```
