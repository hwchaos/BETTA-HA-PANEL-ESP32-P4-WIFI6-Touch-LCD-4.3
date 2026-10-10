#pragma once

#include <stdbool.h>

/* Lays the screen out for the navigation bar position (bottom or left). Called
 * once at boot, before the UI is built; switching needs a restart. */
void ui_geometry_init(bool nav_left);

bool ui_geometry_nav_left(void);
