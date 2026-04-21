#pragma once

// SquareLine-style UI entry.
// 只负责创建/销毁 screens + 暴露指针，不做业务逻辑。

struct ScreenRecog;
struct ScreenEnroll;
struct ScreenManage;

#ifdef __cplusplus
extern "C" {
#endif

// UI thread only
void ui_init(void);

// UI thread only
void ui_deinit(void);

// UI thread only
ScreenRecog*  ui_get_screen_recog(void);
ScreenEnroll* ui_get_screen_enroll(void);
ScreenManage* ui_get_screen_manage(void);

#ifdef __cplusplus
} // extern "C"
#endif
