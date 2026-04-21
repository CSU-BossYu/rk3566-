#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "ui_types.h"

class UiModel {
public:
    UiModel();

    // --------- UI state (UI thread reads, controller writes) ----------
    ui_mode_t mode() const { return mode_.load(); }
    void set_mode(ui_mode_t m) { mode_.store(m); }

    int selected() const { return selected_.load(); }
    void set_selected(int v) { selected_.store(v); }
    void clear_selected() { selected_.store(-1); }

    // actions bitmask (UI->Vision)
    void push_action(uint32_t bit) { actions_.fetch_or(bit); }
    uint32_t poll_actions() { return actions_.exchange(0); }

    // enroll state machine
    enum EnrollState : int { ENROLL_IDLE = 0, ENROLL_WAIT_FACE = 1 };
    int enroll_state() const { return enroll_state_.load(); }
    void set_enroll_state(int s) { enroll_state_.store(s); }

    // enroll name (written by UI thread, read by Vision thread)
    void set_enroll_name(std::string name);
    bool get_enroll_name(std::string& out) const;

    // --------- pending from other threads (Vision->UI) ----------
    void set_status_pending(const char* s);
    bool consume_status(std::string& out);

    void set_manage_list_pending(const std::vector<std::string>& names);
    bool consume_manage_list(std::vector<std::string>& out);

    void set_overlay_pending(const ui_face_box_t* boxes, int n);
    bool consume_overlay(ui_face_box_t out4[4], int& out_n);

    void notify_enroll_done_pending();
    bool consume_enroll_done();

private:
    std::atomic<ui_mode_t> mode_{UI_MODE_RECOG};
    std::atomic<int> selected_{-1};
    std::atomic<uint32_t> actions_{0};
    std::atomic<int> enroll_state_{ENROLL_IDLE};

    // enroll name cross-thread
    mutable std::mutex enroll_mtx_;
    std::string enroll_name_;
    bool enroll_name_valid_{false};

    // pending from Vision->UI
    mutable std::mutex mtx_;
    bool status_pending_{false};
    std::string status_;

    bool manage_pending_{false};
    std::vector<std::string> manage_names_;

    bool overlay_pending_{false};
    ui_face_box_t overlay_boxes_[4]{};
    int overlay_n_{0};

    bool enroll_done_pending_{false};
};
