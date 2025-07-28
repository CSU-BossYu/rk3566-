#pragma once
#include <string>
#include <vector>
#include <opencv2/dnn.hpp>

class ArcFaceCvDnn {
public:
  bool init(const std::string& onnx_path);

  // 输入：RGB HWC uint8，尺寸必须为 112x112（或你主流程传进来的尺寸）
  bool extract(const uint8_t* rgb_hwc_u8, int w, int h, std::vector<float>& feat);

private:
  cv::dnn::Net net_;
  bool inited_ = false;

  std::vector<std::string> out_names_;
  bool dumped_ = false;

  static void l2norm(std::vector<float>& v);
  static void dumpOutShapesOnce(const std::vector<cv::Mat>& outs,
                                const std::vector<std::string>& outNames,
                                bool& dumped_flag);
};
