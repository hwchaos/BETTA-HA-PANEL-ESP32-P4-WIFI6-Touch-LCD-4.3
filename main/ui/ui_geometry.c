#include "ui/ui_geometry.h"

#include "app_config.h"

int16_t g_app_content_box_x = 0;
int16_t g_app_content_box_w = APP_CONTENT_BOX_WIDTH_BASE;
int16_t g_app_content_box_h = APP_CONTENT_BOX_HEIGHT_BASE;

static bool s_nav_left;

void ui_geometry_init(bool nav_left)
{
    s_nav_left = nav_left;
    if (nav_left) {
        g_app_content_box_x = (int16_t)APP_NAV_LEFT_WIDTH;
        g_app_content_box_w = (int16_t)(APP_SCREEN_WIDTH - APP_NAV_LEFT_WIDTH);
        /* The bottom bar is gone: the box reaches the bottom edge. */
        g_app_content_box_h = (int16_t)(APP_SCREEN_HEIGHT - APP_CONTENT_BOX_Y);
    } else {
        g_app_content_box_x = 0;
        g_app_content_box_w = (int16_t)APP_CONTENT_BOX_WIDTH_BASE;
        g_app_content_box_h = (int16_t)APP_CONTENT_BOX_HEIGHT_BASE;
    }
}

bool ui_geometry_nav_left(void)
{
    return s_nav_left;
}
