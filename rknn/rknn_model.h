#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "rknn/include/rknn_api.h"

// 一个最小可用的 RKNN 模型封装：
// - initFromFile()：加载模型 + queryIO
// - runGetOutputs()：inputs_set + run + outputs_get
// - OutputsGuard：自动 outputs_release，避免遗忘
class RknnModel {
public:
    RknnModel() = default;
    virtual ~RknnModel();

    bool initFromFile(const std::string& model_path, uint32_t flags = 0);
    void release();

    bool isInited() const { return inited_; }
    const std::string& lastError() const { return last_error_; }

    // 注意：你的模型是 NHWC: [1, H, W, C]
    int inputWidth() const  { return (in_attr_.n_dims >= 4) ? (int)in_attr_.dims[2] : 0; }
    int inputHeight() const { return (in_attr_.n_dims >= 4) ? (int)in_attr_.dims[1] : 0; }
    int inputChannels() const { return (in_attr_.n_dims >= 4) ? (int)in_attr_.dims[3] : 0; }

    rknn_tensor_format inputFmt()  const { return in_attr_.fmt; }
    rknn_tensor_type   inputType() const { return in_attr_.type; }

    uint32_t numInputs()  const { return n_in_; }
    uint32_t numOutputs() const { return n_out_; }

    void dumpModelInfo(const std::string& tag) const;

    // 统一入口：inputs_set + run + outputs_get
    // want_float=true：让 RKNN runtime 把输出转换成 float32（注意：会有额外开销）
    bool runGetOutputs(rknn_input* inputs, uint32_t n_inputs,
                       std::vector<rknn_output>& outs,
                       bool want_float);

    // RAII：作用域结束自动 rknn_outputs_release
    struct OutputsGuard {
        rknn_context ctx = 0;
        std::vector<rknn_output>* outs = nullptr;
        ~OutputsGuard();
        OutputsGuard() = default;
        OutputsGuard(rknn_context c, std::vector<rknn_output>* o) : ctx(c), outs(o) {}
        OutputsGuard(const OutputsGuard&) = delete;
        OutputsGuard& operator=(const OutputsGuard&) = delete;
    };

protected:
    std::string last_error_;
    bool inited_ = false;
    rknn_context ctx_ = 0;

    uint32_t n_in_ = 0;
    uint32_t n_out_ = 0;

    rknn_tensor_attr in_attr_{};
    std::vector<rknn_tensor_attr> out_attrs_{};

private:
    bool queryIO();
    static bool debugEnabled();
};
