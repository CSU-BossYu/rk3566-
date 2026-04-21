#pragma once
#include "ui_types.h"

struct ScreenRecog;
struct ScreenEnroll;
struct ScreenManage;

class UiNavigator {
public:
    UiNavigator() = default;

    void bind(ScreenRecog* r, ScreenEnroll* e, ScreenManage* m);

    // UI thread only
    void set_mode(ui_mode_t mode);
    ui_mode_t mode() const { return cur_; }

    // UI thread only
    ScreenRecog*  recog()  const { return recog_; }
    ScreenEnroll* enroll() const { return enroll_; }
    ScreenManage* manage() const { return manage_; }

private:
    ui_mode_t cur_{UI_MODE_RECOG};
    ScreenRecog*  recog_{nullptr};
    ScreenEnroll* enroll_{nullptr};
    ScreenManage* manage_{nullptr};
};