#pragma once
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>   // 关键：让 cv::dnn::Net 可见
#include "face_types.h"

class ScrfdDetectorCvDnn {
public:
    ScrfdDetectorCvDnn();
    ~ScrfdDetectorCvDnn();

    /* 固化：320x320 的 det_500m_static_320.onnx */
    bool init(const std::string& onnx_path, int in_w, int in_h);

    /* rgb888: HWC, uint8 */
    bool detect(const uint8_t* rgb888, int w, int h,
                std::vector<FaceBox>& out_faces,
                float score_th = 0.5f, float nms_th = 0.45f);

private:
    struct Head {
        int stride;
        int fm_w;
        int fm_h;
        int rows;      // fm_w*fm_h*2
        cv::Mat score; // [rows,1]
        cv::Mat bbox;  // [rows,4]
        cv::Mat kps;   // [rows,10]
    };

    bool collect_heads_320(const std::vector<cv::Mat>& outs, std::vector<Head>& heads);
    void decode_head(const Head& hd, float score_th, std::vector<FaceBox>& faces);
    static float iou(const FaceBox& a, const FaceBox& b);
    static void nms(std::vector<FaceBox>& faces, float nms_th);

private:
    cv::dnn::Net net_;
    bool inited_{false};

    int in_w_{320};
    int in_h_{320};

    bool dumped_{false};
};
