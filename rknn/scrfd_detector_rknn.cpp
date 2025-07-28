#include "scrfd_detector_rknn.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static constexpr int DET_W = 320;
static constexpr int DET_H = 320;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

uint16_t ScrfdDetectorRknn::f32_to_f16(float x) {
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

bool ScrfdDetectorRknn::init(const std::string& model_path) {
    pool_ready_ = false;

    if (!initFromFile(model_path, 0)) return false;

    if (inputWidth() != DET_W || inputHeight() != DET_H || inputChannels() != 3) return false;
    if (inputFmt() != RKNN_TENSOR_NHWC) return false;

    // 输入池：FP16 blob bytes
    const uint32_t in_bytes = (uint32_t)(DET_W * DET_H * 3 * 2);
    if (!in_pool_.init(in_bytes, 2, 64)) return false;

    // 输出池：block_size = max(output_bytes) among all outputs
    if (out_attrs_.empty()) return false;
    uint32_t max_bytes = 0;
    for (const auto& a : out_attrs_) {
        uint64_t n = 1;
        for (uint32_t i = 0; i < a.n_dims; ++i) n *= (uint64_t)a.dims[i];
        uint64_t bytes = n * sizeof(float);
        if (bytes > max_bytes) max_bytes = (uint32_t)bytes;
    }

    const uint32_t out_num = (uint32_t)out_attrs_.size();
    if (!out_pool_.init(max_bytes, out_num, 64)) return false;

    pool_ready_ = true;
    return true;
}

bool ScrfdDetectorRknn::preprocess_to_fp16(const uint8_t* rgb_hwc_u8, uint16_t* dst_fp16) {
    if (!rgb_hwc_u8 || !dst_fp16) return false;

    constexpr float mean = 127.5f;
    constexpr float inv_std = 1.0f / 128.0f;

    const uint8_t* p = rgb_hwc_u8;
    uint16_t* out = dst_fp16;

    // RGB -> FP16 normalized, NHWC
    for (int i = 0; i < DET_W * DET_H; ++i) {
        float r = ((float)p[0] - mean) * inv_std;
        float g = ((float)p[1] - mean) * inv_std;
        float b = ((float)p[2] - mean) * inv_std;

        out[0] = f32_to_f16(r);
        out[1] = f32_to_f16(g);
        out[2] = f32_to_f16(b);

        p += 3;
        out += 3;
    }
    return true;
}

void ScrfdDetectorRknn::decode_320(const float* s8, const float* s16, const float* s32,
                                  const float* b8, const float* b16, const float* b32,
                                  const float* k8, const float* k16, const float* k32,
                                  std::vector<FaceBox>& faces, float score_th) {
    faces.clear();
    faces.reserve(256);

    constexpr int input = 320;
    constexpr int anchors = 2;

    auto decode_one = [&](const float* score, const float* bbox, const float* kps, int stride) {
        const int fw = input / stride;
        const int fh = input / stride;
        const int loc = fw * fh;
        const int n = loc * anchors;

        for (int i = 0; i < n; ++i) {
            const float sc = score[i];
            if (sc < score_th) continue;

            const int idx = i / anchors;
            const int x = idx % fw;
            const int y = idx / fw;

            const float cx = (x + 0.5f) * stride;
            const float cy = (y + 0.5f) * stride;

            // bbox: l,t,r,b (distance) * stride
            const float l = bbox[i * 4 + 0] * stride;
            const float t = bbox[i * 4 + 1] * stride;
            const float r = bbox[i * 4 + 2] * stride;
            const float b = bbox[i * 4 + 3] * stride;

            FaceBox fb;
            fb.x1 = clampf(cx - l, 0.0f, (float)input);
            fb.y1 = clampf(cy - t, 0.0f, (float)input);
            fb.x2 = clampf(cx + r, 0.0f, (float)input);
            fb.y2 = clampf(cy + b, 0.0f, (float)input);
            fb.score = sc;

            // kps: 10 values per anchor; common SCRFD decode is: (cx + dx*stride, cy + dy*stride)
            // order: le,re,nose,lm,rm
            for (int j = 0; j < 5; ++j) {
                float dx = kps[i * 10 + 2*j + 0] * stride;
                float dy = kps[i * 10 + 2*j + 1] * stride;
                fb.kps[2*j + 0] = clampf(cx + dx, 0.0f, (float)input);
                fb.kps[2*j + 1] = clampf(cy + dy, 0.0f, (float)input);
            }

            faces.push_back(fb);
        }
    };

    decode_one(s8,  b8,  k8,  8);
    decode_one(s16, b16, k16, 16);
    decode_one(s32, b32, k32, 32);
}

float ScrfdDetectorRknn::iou(const FaceBox& a, const FaceBox& b) {
    const float xx1 = std::max(a.x1, b.x1);
    const float yy1 = std::max(a.y1, b.y1);
    const float xx2 = std::min(a.x2, b.x2);
    const float yy2 = std::min(a.y2, b.y2);
    const float w = std::max(0.0f, xx2 - xx1);
    const float h = std::max(0.0f, yy2 - yy1);
    const float inter = w * h;

    const float areaA = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    const float areaB = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    const float uni = areaA + areaB - inter;
    return (uni <= 0.0f) ? 0.0f : (inter / uni);
}

void ScrfdDetectorRknn::nms(std::vector<FaceBox>& faces, float nms_th) {
    if (faces.empty()) return;

    std::sort(faces.begin(), faces.end(),
              [](const FaceBox& a, const FaceBox& b) { return a.score > b.score; });

    std::vector<FaceBox> keep;
    keep.reserve(faces.size());

    std::vector<uint8_t> removed(faces.size(), 0);
    for (size_t i = 0; i < faces.size(); ++i) {
        if (removed[i]) continue;
        keep.push_back(faces[i]);
        for (size_t j = i + 1; j < faces.size(); ++j) {
            if (removed[j]) continue;
            if (iou(keep.back(), faces[j]) > nms_th) removed[j] = 1;
        }
    }
    faces.swap(keep);
}

bool ScrfdDetectorRknn::detect(const uint8_t* rgb_hwc_u8, int w, int h,
                               std::vector<FaceBox>& out_faces,
                               float score_th, float nms_th) {
    out_faces.clear();
    if (!isInited() || !ctx_ || !pool_ready_) return false;
    if (!rgb_hwc_u8 || w != DET_W || h != DET_H) return false;

    // ---- 1) 输入池：FP16 blob ----
    FixedBlockPool::Block bin;
    if (!in_pool_.acquire(bin, 0)) return false;
    FixedBlockPool::AutoBlock in_guard(&in_pool_, bin);

    if (!preprocess_to_fp16(rgb_hwc_u8, (uint16_t*)bin.ptr)) return false;

    rknn_input in{};
    in.index = 0;
    in.buf   = bin.ptr;
    in.size  = bin.size;
    in.type  = RKNN_TENSOR_FLOAT16;
    in.fmt   = RKNN_TENSOR_NHWC;
    in.pass_through = 1;

    if (rknn_inputs_set(ctx_, 1, &in) != RKNN_SUCC) return false;
    if (rknn_run(ctx_, nullptr) != RKNN_SUCC) return false;

    // ---- 2) 输出池：预分配 out_num 个 float buffer ----
    const uint32_t out_num = (uint32_t)out_attrs_.size(); // 你的模型是 9
    std::vector<FixedBlockPool::AutoBlock> out_guards;
    out_guards.reserve(out_num);

    std::vector<rknn_output> outs(out_num);
    std::memset(outs.data(), 0, sizeof(rknn_output) * out_num);

    for (uint32_t i = 0; i < out_num; ++i) {
        FixedBlockPool::Block b;
        if (!out_pool_.acquire(b, 0)) return false;
        out_guards.emplace_back(&out_pool_, b);

        outs[i].index = i;
        outs[i].want_float = 1;
        outs[i].is_prealloc = 1;
        outs[i].buf  = b.ptr;
        outs[i].size = b.size;
    }

    if (rknn_outputs_get(ctx_, out_num, outs.data(), nullptr) != RKNN_SUCC) return false;
    rknn_outputs_release(ctx_, out_num, outs.data());

    // ---- 3) 后处理：0..2 score, 3..5 bbox, 6..8 kps ----
    const float* s8  = (const float*)outs[0].buf;
    const float* s16 = (const float*)outs[1].buf;
    const float* s32 = (const float*)outs[2].buf;

    const float* b8  = (const float*)outs[3].buf;
    const float* b16 = (const float*)outs[4].buf;
    const float* b32 = (const float*)outs[5].buf;

    const float* k8  = (const float*)outs[6].buf;
    const float* k16 = (const float*)outs[7].buf;
    const float* k32 = (const float*)outs[8].buf;

    std::vector<FaceBox> faces;
    decode_320(s8, s16, s32, b8, b16, b32, k8, k16, k32, faces, score_th);
    if (!faces.empty()) nms(faces, nms_th);

    out_faces.swap(faces);
    return true;
}
