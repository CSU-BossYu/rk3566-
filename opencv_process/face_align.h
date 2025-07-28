#pragma once

#include <cstdint>

namespace face_align {

// ArcFace 112x112 五点模板（像素坐标）
// order: left_eye, right_eye, nose, left_mouth, right_mouth
// 这组是常用 InsightFace 模板（适用于 112x112）
static constexpr float kArc112Template[10] = {
    38.2946f, 51.6963f,
    73.5318f, 51.5014f,
    56.0252f, 71.7366f,
    41.5493f, 92.3655f,
    70.7299f, 92.2041f
};

// 输入：整帧 RGB(HWC, u8) + 5点关键点(同一坐标系)
// 输出：对齐后的 RGB112（HWC, u8）
bool align_5pts_rgb112(const uint8_t* rgb_in, int w, int h,
                       const float kps_xy[10],
                       uint8_t out112[112 * 112 * 3]);

} // namespace face_align
