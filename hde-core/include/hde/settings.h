#ifndef HDE_SETTINGS_H
#define HDE_SETTINGS_H
int hde_settings_init(void);
int hde_settings_set(const char *key, const char *value);
const char *hde_settings_get(const char *key, const char *fallback);
void hde_settings_shutdown(void);
#endif
