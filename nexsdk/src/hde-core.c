/* nexsdk/src/hde-core.c — the little that is not a file parser: the release of the SDK itself.
 *
 * Kept in a file of its own so that a program can print `nexsdk_version()` without linking the system,
 * notification, settings and packaging code. */
#include "nexsdk/nexsdk.h"

const char *nexsdk_version(void)
{
    return NEXSDK_VERSION;
}
