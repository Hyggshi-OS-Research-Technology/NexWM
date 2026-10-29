/* mylib — the NexSDK library template: a library that leaves the packaging to the template. */
#include "mylib.h"

#include <string.h>

const char *mylib_greeting(void)
{
    return "Hello from mylib";
}

int mylib_contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}
