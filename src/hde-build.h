/* hde-build.h — HDE_VERSION: the commit HDE was built from, e.g. "1a2b3c4 2026-10-05", shown in Settings > About,
 * by `--version` and at the top of the session log, so it is easy to see whether the programs that run are the ones
 * just built. The Makefile writes it to build/hde-version.h (scripts/hde-version.sh: from git, or from data/version
 * in a GitHub ZIP download); "unknown" when the sources are compiled some other way. */
#ifndef HDE_BUILD_H
#define HDE_BUILD_H

#if defined(__has_include)
#if __has_include("hde-version.h")
#include "hde-version.h"
#endif
#endif
#ifndef HDE_VERSION
#define HDE_VERSION "unknown"
#endif
/* the release shown in Settings > About next to the build */
#define HDE_RELEASE "1.0"

#endif
