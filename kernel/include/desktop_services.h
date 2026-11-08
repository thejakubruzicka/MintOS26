#ifndef MINTOS26_DESKTOP_SERVICES_H
#define MINTOS26_DESKTOP_SERVICES_H
#include "desktop_abi.h"

void mint_desktop_settings_init(void);
int mint_desktop_settings_get(MINT_DESKTOP_SETTINGS *out);
int mint_desktop_settings_set(const MINT_DESKTOP_SETTINGS *in);

#endif
