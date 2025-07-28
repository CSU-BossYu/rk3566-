#pragma once
#include <string>
#include <vector>
#include <cstdint>

#include "rknn_model.h"
#include "opencv_process/face_types.h"
#include "utils/fixed_block_pool.h"

class ScrfdDetectorRknn : public RknnModel {
public:
    bool init(const std::string& model_path);

    // 输入：RGB888 HWC(320x320)
    // 输出：FaceBox 坐标/关键点均在 320x320 空间
    bool detect(const uint8_t* rgb_hwc_u8, int w, int h,
                std::vector<FaceBox>& out_faces,
                float score_th = 0.5f, float nms_th = 0.45f);

private:
    FixedBlockPool in_pool_;
    FixedBlockPool out_pool_;
    bool pool_ready_ = false;

    // preprocess: (x-127.5)/128 -> FP16 NHWC 写入 dst
    // 注意：这里不 swapRB（你已确认输入是 RGB）
    bool preprocess_to_fp16(const uint8_t* rgb_hwc_u8, uint16_t* dst_fp16);

    void decode_320(const float* s8,  const float* s16,  const float* s32,
                    const float* b8,  const float* b16,  const float* b32,
                    const float* k8,  const float* k16,  const float* k32,
                    std::vector<FaceBox>& faces, float score_th);

    static float iou(const FaceBox& a, const FaceBox& b);
    static void nms(std::vector<FaceBox>& faces, float nms_th);

    static inline uint16_t f32_to_f16(float x);
};
