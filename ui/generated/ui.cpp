#include "ui.h"

#include "ui/screens/screen_recog.h"
#include "ui/screens/screen_enroll.h"
#include "ui/screens/screen_manage.h"

#include <lvgl.h>

static ScreenRecog*  g_recog  = nullptr;
static ScreenEnroll* g_enroll = nullptr;
static ScreenManage* g_manage = nullptr;

void ui_init(void) {
    if (g_recog || g_enroll || g_manage) return;

    // 这里就是 SquareLine 生成代码通常干的事：
    // 1) create all screens
    // 2) init default screen (由上层决定 load 哪个)

    g_recog  = screen_recog_create();
    g_enroll = screen_enroll_create();
    g_manage = screen_manage_create();
}

void ui_deinit(void) {
    // 这里是 SquareLine 通常不会自动生成的，但我们工程需要“可控释放”
    if (g_recog)  { screen_recog_destroy(g_recog);   g_recog = nullptr; }
    if (g_enroll) { screen_enroll_destroy(g_enroll); g_enroll = nullptr; }
    if (g_manage) { screen_manage_destroy(g_manage); g_manage = nullptr; }
}

ScreenRecog* ui_get_screen_recog(void) { return g_recog; }
ScreenEnroll* ui_get_screen_enroll(void) { return g_enroll; }
ScreenManage* ui_get_screen_manage(void) { return g_manage; }
