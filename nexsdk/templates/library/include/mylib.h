/* mylib.h — the whole API of the library template. */
#ifndef MYLIB_H
#define MYLIB_H

#ifdef __cplusplus
extern "C" {
#endif

const char *mylib_greeting(void);
int mylib_contains(const char *haystack, const char *needle);

#ifdef __cplusplus
}
#endif
#endif
