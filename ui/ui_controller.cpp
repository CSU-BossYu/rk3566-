#include "ui_controller.h"
#include "ui_model.h"
#include "ui_navigator.h"
#include "ui_app.h"

#include "screens/screen_recog.h"
#include "screens/screen_enroll.h"
#include "screens/screen_manage.h"

#include <algorithm>

static constexpr uint32_t ACT_START_ENROLL  = UI_ACT_START_ENROLL;
static constexpr uint32_t ACT_DELETE        = UI_ACT_DELETE;
static constexpr uint32_t ACT_CANCEL_ENROLL = UI_ACT_CANCEL_ENROLL;

UiController::UiController(UiModel* model, UiNavigator* nav,
                           ScreenRecog* r, ScreenEnroll* e, ScreenManage* m)
    : model_(model), nav_(nav), recog_(r), enroll_(e), manage_(m) {}

const char* UiController::mode_text_(ui_mode_t m) const {
    switch (m) {
    case UI_MODE_RECOG:  return "MODE: RECOG";
    case UI_MODE_ENROLL: return "MODE: ENROLL";
    case UI_MODE_MANAGE: return "MODE: MANAGE";
    default:             return "MODE: ?";
    }
}

void UiController::wire_events() {
    // nav buttons on each screen
    if (recog_) {
        lv_obj_add_event_cb(recog_->btn_left,  on_btn_left,  LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(recog_->btn_right, on_btn_right, LV_EVENT_CLICKED, this);
    }
    if (enroll_) {
        lv_obj_add_event_cb(enroll_->btn_left,  on_btn_left,  LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(enroll_->btn_right, on_btn_right, LV_EVENT_CLICKED, this);

        lv_obj_add_event_cb(enroll_->btn_action, on_enroll_action, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(enroll_->ta_name, on_enroll_ta, LV_EVENT_ALL, this);
    }
    if (manage_) {
        lv_obj_add_event_cb(manage_->btn_left,  on_btn_left,  LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(manage_->btn_right, on_btn_right, LV_EVENT_CLICKED, this);

        lv_obj_add_event_cb(manage_->btn_action, on_manage_delete, LV_EVENT_CLICKED, this);
    }
}

void UiController::apply() {
    const ui_mode_t m = model_->mode();

    // update mode labels
    if (recog_)  screen_recog_set_mode_text(recog_,  mode_text_(m));
    if (enroll_) screen_enroll_set_mode_text(enroll_, mode_text_(m));
    if (manage_) screen_manage_set_mode_text(manage_, mode_text_(m));

    // enroll action text
    if (m == UI_MODE_ENROLL && enroll_) {
        const bool wait = (model_->enroll_state() == UiModel::ENROLL_WAIT_FACE);
        screen_enroll_set_action_text(enroll_, wait ? "Cancel" : "Start");
    }

    // manage delete enable
    if (m == UI_MODE_MANAGE && manage_) {
        screen_manage_set_delete_enabled(manage_, model_->selected() >= 0);
    }

    // hide kb if not in enroll
    if (m != UI_MODE_ENROLL && enroll_) {
        model_->set_enroll_state(UiModel::ENROLL_IDLE);
        screen_enroll_kb_hide(enroll_);
    }
}

// ---------------- thunks ----------------
void UiController::on_btn_left(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (self) self->switch_left_();
}
void UiController::on_btn_right(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (self) self->switch_right_();
}

void UiController::on_enroll_action(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (self) self->enroll_action_();
}
void UiController::on_enroll_ta(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (!self) return;
    self->enroll_ta_event_(lv_event_get_code(e));
}

void UiController::on_manage_delete(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (self) self->manage_delete_();
}

void UiController::on_manage_list_item(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (!self) return;
    // user_data is ScreenManageListItemCtx*
    auto* ctx = (ScreenManageListItemCtx*)lv_event_get_user_data(e);
    if (!ctx) return;
    self->manage_list_item_(ctx->idx);
}

// ---------------- helpers ----------------

void UiController::switch_left_() {
    ui_mode_t m = model_->mode();
    ui_mode_t nm;
    // 3-screen loop: RECOG <-> ENROLL <-> MANAGE
    if (m == UI_MODE_RECOG) nm = UI_MODE_MANAGE;
    else nm = (ui_mode_t)(m - 1);
    model_->set_mode(nm);
    if (nav_) nav_->set_mode(nm);
}

void UiController::switch_right_() {
    ui_mode_t m = model_->mode();
    ui_mode_t nm;
    if (m == UI_MODE_MANAGE) nm = UI_MODE_RECOG;
    else nm = (ui_mode_t)(m + 1);
    model_->set_mode(nm);
    if (nav_) nav_->set_mode(nm);
}

void UiController::enroll_ta_event_(lv_event_code_t code) {
    if (!enroll_ || !enroll_->kb || !enroll_->ta_name) return;

    if (code == LV_EVENT_FOCUSED) {
        screen_enroll_kb_show_for(enroll_, enroll_->ta_name);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
        screen_enroll_kb_hide(enroll_);
    }
}

void UiController::enroll_action_() {
    if (!enroll_) return;

    const int st = model_->enroll_state();
    if (st == UiModel::ENROLL_IDLE) {
        const char* txt = lv_textarea_get_text(enroll_->ta_name);
        std::string name = txt ? txt : "";
        model_->set_enroll_name(std::move(name));

        std::string chk;
        if (!model_->get_enroll_name(chk)) {
            model_->set_status_pending("ENROLL: name empty");
            return;
        }

        model_->push_action(ACT_START_ENROLL);
        model_->set_enroll_state(UiModel::ENROLL_WAIT_FACE);
        model_->set_status_pending("ENROLL: waiting face");
        screen_enroll_kb_hide(enroll_);
    } else {
        model_->push_action(ACT_CANCEL_ENROLL);
        model_->set_enroll_state(UiModel::ENROLL_IDLE);
        model_->set_status_pending("ENROLL: cancelled");
        screen_enroll_kb_hide(enroll_);
    }
}

void UiController::manage_list_item_(int idx) {
    model_->set_selected(idx);
    if (manage_) {
        screen_manage_list_set_selected(manage_, idx);
        screen_manage_set_delete_enabled(manage_, idx >= 0);
    }
}

void UiController::manage_delete_() {
    if (model_->selected() < 0) {
        model_->set_status_pending("DELETE: select one");
        return;
    }
    open_delete_confirm_();
}

void UiController::open_delete_confirm_() {
    if (!manage_ || mbox_closing_) return;
    if (manage_->mbox) return;

    static const char* btns[] = {"Cancel", "OK", ""};
    lv_obj_t* parent = lv_layer_top();
    manage_->mbox = lv_msgbox_create(parent, "Confirm", "Delete selected person?", btns, true);
    lv_obj_center(manage_->mbox);
    lv_obj_move_foreground(manage_->mbox);

    lv_obj_add_event_cb(manage_->mbox, on_mbox_deleted, LV_EVENT_DELETE, this);

    lv_obj_t* btnm = lv_msgbox_get_btns(manage_->mbox);
    if (btnm) {
        lv_obj_add_event_cb(btnm, on_mbox_btnmatrix, LV_EVENT_VALUE_CHANGED, this);
    }
}

void UiController::on_mbox_deleted(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (!self || !self->manage_) return;
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    if (self->manage_->mbox == lv_event_get_target(e)) self->manage_->mbox = nullptr;
    self->mbox_closing_ = false;
    self->mbox_ok_pending_ = 0;
}

void UiController::on_mbox_btnmatrix(lv_event_t* e) {
    auto* self = (UiController*)lv_event_get_user_data(e);
    if (!self || !self->manage_ || !self->manage_->mbox) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t* btnm = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(btnm);
    self->mbox_ok_pending_ = (id == 1) ? 1 : 0; // OK index=1

    if (self->mbox_closing_) return;
    self->mbox_closing_ = true;

    lv_async_call(mbox_async_close, self);
}

void UiController::mbox_async_close(void* p) {
    auto* self = (UiController*)p;
    if (!self || !self->manage_) return;

    if (self->mbox_ok_pending_ == 1) {
        self->mbox_ok_pending_ = 0;
        self->model_->push_action(ACT_DELETE);
    }

    if (self->manage_->mbox && lv_obj_is_valid(self->manage_->mbox)) {
        lv_obj_del_async(self->manage_->mbox);
    }
}