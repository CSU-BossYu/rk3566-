#pragma once
#include <string>
#include <vector>
#include <cstdint>

#include "rknn_model.h"
#include "utils/fixed_block_pool.h"

class ArcFaceRknn : public RknnModel {
public:
    bool init(const std::string& model_path);

    // 输入：RGB888 HWC(112x112)
    // 输出：512 float（本函数会做 L2 normalize）
    bool inferEmbeddingHWC(const uint8_t* rgb_hwc_u8, int w, int h,
                           std::vector<float>& embedding_norm);

private:
    // 输入池：按模型 inputType() 自适配
    // - 新模型通常 UINT8: 112*112*3*1
    // - 兼容旧模型 FP16: 112*112*3*2
    FixedBlockPool in_pool_;

    // 输出池：float[512]
    FixedBlockPool out_pool_;

    bool pool_ready_ = false;

    static void l2_normalize_512(float* v);
    static inline uint16_t f32_to_f16(float x);
};
