#include "rknn_model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

static inline const char* fmt_str(int f) {
    switch (f) {
    case RKNN_TENSOR_NCHW: return "NCHW";
    case RKNN_TENSOR_NHWC: return "NHWC";
    default: return "UNDEF";
    }
}
static inline const char* type_str(int t) {
    switch (t) {
    case RKNN_TENSOR_FLOAT32: return "FP32";
    case RKNN_TENSOR_FLOAT16: return "FP16";
    case RKNN_TENSOR_INT8:    return "INT8";
    case RKNN_TENSOR_UINT8:   return "UINT8";
    case RKNN_TENSOR_INT16:   return "INT16";
    case RKNN_TENSOR_UINT16:  return "UINT16";
    default: return "UNKNOWN";
    }
}

bool RknnModel::debugEnabled() {
    const char* s = std::getenv("RKNN_DEBUG");
    return (s && std::atoi(s) != 0);
}

#define RKNN_LOG(...)  do { if (RknnModel::debugEnabled()) std::fprintf(stderr, __VA_ARGS__); } while(0)
#define RKNN_ERR(...)  do { std::fprintf(stderr, __VA_ARGS__); } while(0)

RknnModel::~RknnModel() { release(); }

RknnModel::OutputsGuard::~OutputsGuard() {
    if (ctx && outs && !outs->empty()) {
        rknn_outputs_release(ctx, (uint32_t)outs->size(), outs->data());
    }
}

void RknnModel::release() {
    if (ctx_) {
        rknn_destroy(ctx_);
        ctx_ = 0;
    }
    inited_ = false;
    n_in_ = n_out_ = 0;
    out_attrs_.clear();
    std::memset(&in_attr_, 0, sizeof(in_attr_));
    last_error_.clear();
}

static bool read_file_all(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streamsize sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    if (sz <= 0) return false;
    out.resize((size_t)sz);
    return (bool)ifs.read((char*)out.data(), sz);
}

bool RknnModel::initFromFile(const std::string& model_path, uint32_t flags) {
    release();

    std::vector<uint8_t> model;
    if (!read_file_all(model_path, model)) {
        last_error_ = "read model file failed: " + model_path;
        return false;
    }

    RKNN_LOG("[RKNN] initFromFile: %s flags=%u bytes=%zu\n",
             model_path.c_str(), flags, model.size());

    int ret = rknn_init(&ctx_, model.data(), model.size(), flags, nullptr);
    if (ret != RKNN_SUCC) {
        last_error_ = "rknn_init failed ret=" + std::to_string(ret);
        ctx_ = 0;
        return false;
    }

    if (!queryIO()) {
        release();
        return false;
    }

    if (debugEnabled()) {
        rknn_sdk_version ver{};
        ret = rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
        if (ret == RKNN_SUCC) {
            RKNN_LOG("[RKNN] SDK_VERSION: drv=%s api=%s\n", ver.drv_version, ver.api_version);
        }
    }

    inited_ = true;
    return true;
}

bool RknnModel::queryIO() {
    last_error_.clear();

    rknn_input_output_num io_num{};
    int ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        last_error_ = "rknn_query(IN_OUT_NUM) failed ret=" + std::to_string(ret);
        return false;
    }
    n_in_ = io_num.n_input;
    n_out_ = io_num.n_output;

    if (n_in_ < 1 || n_out_ < 1) {
        last_error_ = "invalid in/out num";
        return false;
    }

    std::memset(&in_attr_, 0, sizeof(in_attr_));
    in_attr_.index = 0;
    ret = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_));
    if (ret != RKNN_SUCC) {
        last_error_ = "rknn_query(INPUT_ATTR) failed ret=" + std::to_string(ret);
        return false;
    }

    out_attrs_.resize(n_out_);
    for (uint32_t i = 0; i < n_out_; ++i) {
        std::memset(&out_attrs_[i], 0, sizeof(rknn_tensor_attr));
        out_attrs_[i].index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attrs_[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            last_error_ = "rknn_query(OUTPUT_ATTR) failed idx=" + std::to_string(i) +
                          " ret=" + std::to_string(ret);
            return false;
        }
    }

    RKNN_LOG("[RKNN] queryIO: in_num=%u out_num=%u in=%dx%dx%d fmt=%s type=%s\n",
             n_in_, n_out_,
             inputWidth(), inputHeight(), inputChannels(),
             fmt_str(in_attr_.fmt), type_str(in_attr_.type));
    return true;
}

void RknnModel::dumpModelInfo(const std::string& tag) const {
    RKNN_ERR("----- RKNN Model Info (%s) -----\n", tag.c_str());
    RKNN_ERR("[Input] idx=0 name=%s n_dims=%d dims=[",
             in_attr_.name, in_attr_.n_dims);
    for (uint32_t  i = 0; i < in_attr_.n_dims; ++i) {
        RKNN_ERR("%d%s", in_attr_.dims[i], (i + 1 == in_attr_.n_dims) ? "]" : ",");
    }
    RKNN_ERR(" fmt=%s type=%s qnt_type=%d w_stride=%d size=%d size_with_stride=%d\n",
             fmt_str(in_attr_.fmt), type_str(in_attr_.type),
             (int)in_attr_.qnt_type, in_attr_.w_stride,
             in_attr_.size, in_attr_.size_with_stride);

    for (uint32_t i = 0; i < out_attrs_.size(); ++i) {
        const auto& a = out_attrs_[i];
        RKNN_ERR("[Output] idx=%u name=%s n_dims=%d dims=[",
                 i, a.name, a.n_dims);
        for (uint32_t  d = 0; d < a.n_dims; ++d) {
            RKNN_ERR("%d%s", a.dims[d], (d + 1 == a.n_dims) ? "]" : ",");
        }
        RKNN_ERR(" type=%s qnt_type=%d size=%d size_with_stride=%d\n",
                 type_str(a.type), (int)a.qnt_type, a.size, a.size_with_stride);
    }
    RKNN_ERR("---------------------------------\n");
}

bool RknnModel::runGetOutputs(rknn_input* inputs, uint32_t n_inputs,
                             std::vector<rknn_output>& outs,
                             bool want_float) {
    last_error_.clear();
    outs.clear();

    if (!inited_ || !ctx_) {
        last_error_ = "model not inited";
        return false;
    }
    if (!inputs || n_inputs == 0) {
        last_error_ = "invalid inputs";
        return false;
    }

    int ret = rknn_inputs_set(ctx_, n_inputs, inputs);
    if (ret != RKNN_SUCC) {
        last_error_ = "rknn_inputs_set failed ret=" + std::to_string(ret);
        return false;
    }

    ret = rknn_run(ctx_, nullptr);
    if (ret != RKNN_SUCC) {
        last_error_ = "rknn_run failed ret=" + std::to_string(ret);
        return false;
    }

    outs.assign(n_out_, rknn_output{});
    for (uint32_t i = 0; i < n_out_; ++i) {
        outs[i].index = i;
        outs[i].want_float = want_float ? 1 : 0;
        outs[i].is_prealloc = 0;
    }

    ret = rknn_outputs_get(ctx_, n_out_, outs.data(), nullptr);
    if (ret != RKNN_SUCC) {
        last_error_ = "rknn_outputs_get failed ret=" + std::to_string(ret);
        outs.clear();
        return false;
    }

    // 如需排查 size/指针，仅在 RKNN_DEBUG=1 时打印
    if (debugEnabled()) {
        for (uint32_t i = 0; i < n_out_; ++i) {
            RKNN_LOG("[RKNN] OUT%u buf=%p size=%u want_float=%d\n",
                     i, outs[i].buf, outs[i].size, outs[i].want_float);
        }
    }
    return true;
}
