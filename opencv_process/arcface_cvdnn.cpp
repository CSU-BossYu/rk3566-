#include "arcface_cvdnn.h"
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <iostream>

bool ArcFaceCvDnn::init(const std::string& onnx_path) {
  try {
    net_ = cv::dnn::readNetFromONNX(onnx_path);
    net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

    out_names_ = net_.getUnconnectedOutLayersNames();
    if (out_names_.empty()) {
      std::cerr << "[ArcFaceCvDnn] no unconnected output layers.\n";
      return false;
    }

    inited_ = true;
    dumped_ = false;
    return true;
  } catch (const std::exception& e) {
    std::cerr << "[ArcFaceCvDnn] init failed: " << e.what() << "\n";
    inited_ = false;
    return false;
  }
}

void ArcFaceCvDnn::l2norm(std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += (double)x * (double)x;
  s = std::sqrt(s) + 1e-12;
  for (float& x : v) x = (float)(x / s);
}

void ArcFaceCvDnn::dumpOutShapesOnce(const std::vector<cv::Mat>& outs,
                                     const std::vector<std::string>& outNames,
                                     bool& dumped_flag) {
  if (dumped_flag) return;
  dumped_flag = true;

  std::cerr << "\n[ArcFaceCvDnn] ===== ONNX output dump (first forward) =====\n";
  std::cerr << "[ArcFaceCvDnn] outNames(" << outNames.size() << "):\n";
  for (size_t i = 0; i < outNames.size(); ++i) {
    std::cerr << "  - " << outNames[i] << "\n";
  }
  std::cerr << "[ArcFaceCvDnn] outs(" << outs.size() << "):\n";
  for (size_t i = 0; i < outs.size(); ++i) {
    const cv::Mat& o = outs[i];
    std::cerr << "  [" << i << "]";
    if (i < outNames.size()) std::cerr << " name=" << outNames[i];
    std::cerr << " dims=" << o.dims << " shape=";
    if (o.dims > 0) {
      std::cerr << "[";
      for (int d = 0; d < o.dims; ++d) {
        std::cerr << o.size[d] << (d + 1 == o.dims ? "" : ",");
      }
      std::cerr << "]";
    }
    std::cerr << " type=" << o.type() << " total=" << o.total() << "\n";
  }
  std::cerr << "[ArcFaceCvDnn] ===========================================\n\n";
}

bool ArcFaceCvDnn::extract(const uint8_t* rgb_hwc_u8, int w, int h, std::vector<float>& feat) {
  if (!inited_) return false;
  if (!rgb_hwc_u8 || w <= 0 || h <= 0) return false;

  // 你的主链路（RGA）已经输出 RGB，所以这里 swapRB 必须是 false
  // ArcFace 常见预处理： (x - 127.5) / 128
  cv::Mat img(h, w, CV_8UC3, const_cast<uint8_t*>(rgb_hwc_u8));
  cv::Mat blob = cv::dnn::blobFromImage(
      img, 1.0 / 128.0, cv::Size(w, h),
      cv::Scalar(127.5, 127.5, 127.5),
      false /*swapRB*/, false /*crop*/);

  net_.setInput(blob);

  // 强制 forward 到所有输出名（避免 OpenCV 默认拿到中间特征）
  std::vector<cv::Mat> outs;
  net_.forward(outs, out_names_);

  dumpOutShapesOnce(outs, out_names_, dumped_);

  // 从多个输出里，选 total==512 的那个作为 embedding
  int best = -1;
  for (int i = 0; i < (int)outs.size(); ++i) {
    const cv::Mat& o = outs[i];
    if (o.empty()) continue;
    if (o.type() != CV_32F) continue;
    if ((int)o.total() == 512) {
      best = i;
      break;
    }
  }

  if (best < 0) {
    std::cerr << "[ArcFaceCvDnn] cannot find 512-d embedding output. "
                 "Check dump above and pick correct output name.\n";
    return false;
  }

  const cv::Mat& out = outs[best];
  feat.assign((float*)out.datastart, (float*)out.dataend);
  if ((int)feat.size() != 512) {
    std::cerr << "[ArcFaceCvDnn] selected output total=512 but feat.size()=" << feat.size() << "\n";
    return false;
  }

  l2norm(feat);
  return true;
}
