#include <stdint.h>
#include "../include/desktop_services.h"

static MINT_DESKTOP_SETTINGS current_settings;

void mint_desktop_settings_init(void) {
    current_settings.version = MINT_DESKTOP_SETTINGS_VERSION;
    current_settings.theme = MINT_THEME_LIGHT;
    current_settings.accent = MINT_ACCENT_MINT;
    current_settings.ui_scale = 1u;
    current_settings.wallpaper = 0u;
    current_settings.flags = 0u;
    current_settings.generation = 1u;
    current_settings.reserved = 0u;
}

int mint_desktop_settings_get(MINT_DESKTOP_SETTINGS *out) {
    if (!out) return 0;
    *out = current_settings;
    return 1;
}

int mint_desktop_settings_set(const MINT_DESKTOP_SETTINGS *in) {
    if (!in || in->version != MINT_DESKTOP_SETTINGS_VERSION ||
        in->theme > MINT_THEME_WARM || in->accent > MINT_ACCENT_ROSE ||
        in->ui_scale < 1u || in->ui_scale > 3u || in->wallpaper > 2u ||
        (in->flags & ~MINT_DESKTOP_SETTING_REDUCE_MOTION) != 0u || in->reserved != 0u)
        return 0;
    uint32_t generation = current_settings.generation + 1u;
    if (generation == 0u) generation = 1u;
    current_settings = *in;
    current_settings.generation = generation;
    return 1;
}
