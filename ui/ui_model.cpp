#include "ui_model.h"
#include <algorithm>
#include <cstring>

UiModel::UiModel() = default;

void UiModel::set_enroll_name(std::string name) {
    // trim
    auto ltrim = [](std::string& s) {
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i) s.erase(0, i);
    };
    auto rtrim = [](std::string& s) {
        while (!s.empty()) {
            char c = s.back();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') s.pop_back();
            else break;
        }
    };
    ltrim(name); rtrim(name);

    std::lock_guard<std::mutex> lk(enroll_mtx_);
    enroll_name_ = std::move(name);
    enroll_name_valid_ = !enroll_name_.empty();
}

bool UiModel::get_enroll_name(std::string& out) const {
    std::lock_guard<std::mutex> lk(enroll_mtx_);
    if (!enroll_name_valid_) return false;
    out = enroll_name_;
    return true;
}

void UiModel::set_status_pending(const char* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lk(mtx_);
    status_ = s;
    status_pending_ = true;
}

bool UiModel::consume_status(std::string& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!status_pending_) return false;
    out = status_;
    status_pending_ = false;
    return true;
}

void UiModel::set_manage_list_pending(const std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lk(mtx_);
    manage_names_ = names;
    manage_pending_ = true;
}

bool UiModel::consume_manage_list(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!manage_pending_) return false;
    out = manage_names_;
    manage_pending_ = false;
    return true;
}

void UiModel::set_overlay_pending(const ui_face_box_t* boxes, int n) {
    if (n < 0) n = 0;
    if (n > 4) n = 4;

    std::lock_guard<std::mutex> lk(mtx_);
    overlay_n_ = n;
    for (int i = 0; i < n; ++i) overlay_boxes_[i] = boxes[i];
    overlay_pending_ = true;
}

bool UiModel::consume_overlay(ui_face_box_t out4[4], int& out_n) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!overlay_pending_) return false;
    out_n = overlay_n_;
    for (int i = 0; i < out_n; ++i) out4[i] = overlay_boxes_[i];
    overlay_pending_ = false;
    return true;
}

void UiModel::notify_enroll_done_pending() {
    std::lock_guard<std::mutex> lk(mtx_);
    enroll_done_pending_ = true;
}

bool UiModel::consume_enroll_done() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!enroll_done_pending_) return false;
    enroll_done_pending_ = false;
    return true;
}
