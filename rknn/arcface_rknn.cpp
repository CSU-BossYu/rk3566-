#include "arcface_rknn.h"
#include <cmath>
#include <cstring>

static constexpr int ARC_W = 112;
static constexpr int ARC_H = 112;

static inline uint32_t bytes_per_elem(rknn_tensor_type t) {
    switch (t) {
    case RKNN_TENSOR_UINT8:   return 1;
    case RKNN_TENSOR_INT8:    return 1;
    case RKNN_TENSOR_FLOAT16: return 2;
    case RKNN_TENSOR_FLOAT32: return 4;
    default: return 0;
    }
}

uint16_t ArcFaceRknn::f32_to_f16(float x) {
#if defined(__aarch64__) || defined(__ARM_FP16_FORMAT_IEEE)
    __fp16 h = (__fp16)x;
    uint16_t u;
    std::memcpy(&u, &h, sizeof(u));
    return u;
#else
    (void)x;
    return 0;
#endif
}

void ArcFaceRknn::l2_normalize_512(float* v) {
    double s = 0.0;
    for (int i = 0; i < 512; ++i) s += (double)v[i] * (double)v[i];
    double n = std::sqrt(s) + 1e-12;
    for (int i = 0; i < 512; ++i) v[i] = (float)((double)v[i] / n);
}

bool ArcFaceRknn::init(const std::string& model_path) {
    pool_ready_ = false;

    if (!initFromFile(model_path, 0)) return false;

    // 强约束：输入维度/格式不匹配直接失败
    if (inputWidth() != ARC_W || inputHeight() != ARC_H || inputChannels() != 3) return false;
    if (inputFmt() != RKNN_TENSOR_NHWC) return false;

    // 输入池：按模型 inputType() 自适配
    const uint32_t bpe = bytes_per_elem(inputType());
    if (bpe == 0) return false;

    const uint32_t in_bytes  = (uint32_t)(ARC_W * ARC_H * 3 * bpe);
    const uint32_t out_bytes = (uint32_t)(512 * sizeof(float));

    if (!in_pool_.init(in_bytes, 2, 64)) return false;
    if (!out_pool_.init(out_bytes, 2, 64)) return false;

    pool_ready_ = true;
    return true;
}

bool ArcFaceRknn::inferEmbeddingHWC(const uint8_t* rgb_hwc_u8, int w, int h,
                                   std::vector<float>& embedding_norm) {
    embedding_norm.clear();
    if (!isInited() || !ctx_ || !pool_ready_) return false;
    if (!rgb_hwc_u8 || w != ARC_W || h != ARC_H) return false;

    // ---- 1) 申请输入 block ----
    FixedBlockPool::Block bin;
    if (!in_pool_.acquire(bin, 0)) return false;
    FixedBlockPool::AutoBlock in_guard(&in_pool_, bin);

    // 新模型（NPU 归一化）：UINT8 直喂（RGB），pass_through=0
    if (inputType() == RKNN_TENSOR_UINT8) {
        std::memcpy(bin.ptr, rgb_hwc_u8, (size_t)ARC_W * ARC_H * 3);
    }
    // 兼容旧模型：FP16 + CPU 手动归一化，pass_through=1
    else if (inputType() == RKNN_TENSOR_FLOAT16) {
        constexpr float mean = 127.5f;
        constexpr float inv_std = 1.0f / 128.0f;

        uint16_t* fp16 = (uint16_t*)bin.ptr;
        const uint8_t* p = rgb_hwc_u8;
        for (int i = 0; i < ARC_W * ARC_H; ++i) {
            float r = ((float)p[0] - mean) * inv_std;
            float g = ((float)p[1] - mean) * inv_std;
            float b = ((float)p[2] - mean) * inv_std;
            fp16[0] = f32_to_f16(r);
            fp16[1] = f32_to_f16(g);
            fp16[2] = f32_to_f16(b);
            p += 3;
            fp16 += 3;
        }
    } else {
        return false;
    }

    rknn_input in{};
    in.index = 0;
    in.buf   = bin.ptr;
    in.size  = bin.size;
    in.type  = inputType();
    in.fmt   = RKNN_TENSOR_NHWC;

    // UINT8 新模型：pass_through=0 -> 让 RKNN runtime 做 mean/std
    // FP16 旧模型：pass_through=1 -> 你已手动 normalize
    in.pass_through = (inputType() == RKNN_TENSOR_UINT8) ? 0 : 1;

    if (rknn_inputs_set(ctx_, 1, &in) != RKNN_SUCC) return false;
    if (rknn_run(ctx_, nullptr) != RKNN_SUCC) return false;

    // ---- 2) 申请输出 block（float[512]）并预分配 ----
    FixedBlockPool::Block bout;
    if (!out_pool_.acquire(bout, 0)) return false;
    FixedBlockPool::AutoBlock out_guard(&out_pool_, bout);

    rknn_output out{};
    out.index = 0;
    out.want_float = 1;
    out.is_prealloc = 1;
    out.buf  = bout.ptr;
    out.size = bout.size;

    if (rknn_outputs_get(ctx_, 1, &out, nullptr) != RKNN_SUCC) return false;
    rknn_outputs_release(ctx_, 1, &out);

    // ---- 3) 拷贝 + L2 normalize ----
    embedding_norm.resize(512);
    std::memcpy(embedding_norm.data(), bout.ptr, 512 * sizeof(float));
    l2_normalize_512(embedding_norm.data());
    return true;
}
