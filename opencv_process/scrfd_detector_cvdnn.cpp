#include "scrfd_detector_cvdnn.h"
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstdio>
#include <cmath>

ScrfdDetectorCvDnn::ScrfdDetectorCvDnn() {}
ScrfdDetectorCvDnn::~ScrfdDetectorCvDnn() {}

bool ScrfdDetectorCvDnn::init(const std::string& onnx_path, int in_w, int in_h)
{
    in_w_ = in_w;
    in_h_ = in_h;

    /* 你要求“写死最简单版本”：这里强制 320x320 */
    in_w_ = 320;
    in_h_ = 320;

    try {
        net_ = cv::dnn::readNetFromONNX(onnx_path);
    } catch(const std::exception& e) {
        printf("[ScrfdDetectorCvDnn] readNetFromONNX exception: %s\n", e.what());
        return false;
    }

    if(net_.empty()) {
        printf("[ScrfdDetectorCvDnn] net empty, path=%s\n", onnx_path.c_str());
        return false;
    }

    /* CPU 推理 */
    net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

    inited_ = true;
    dumped_ = false;
    return true;
}

static inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

bool ScrfdDetectorCvDnn::collect_heads_320(const std::vector<cv::Mat>& outs, std::vector<Head>& heads)
{
    heads.clear();

    Head h8  { 8, 40, 40, 3200, cv::Mat(), cv::Mat(), cv::Mat() };
    Head h16 {16, 20, 20,  800, cv::Mat(), cv::Mat(), cv::Mat() };
    Head h32 {32, 10, 10,  200, cv::Mat(), cv::Mat(), cv::Mat() };


    bool got8s=false, got8b=false, got8k=false;
    bool got16s=false, got16b=false, got16k=false;
    bool got32s=false, got32b=false, got32k=false;

    for(const auto& m : outs) {
        if(m.dims != 2) continue;
        int rows = m.size[0];
        int cols = m.size[1];

        if(rows==h8.rows && cols==1 && !got8s) { h8.score=m; got8s=true; continue; }
        if(rows==h8.rows && cols==4 && !got8b) { h8.bbox =m; got8b=true; continue; }
        if(rows==h8.rows && cols==10 && !got8k){ h8.kps  =m; got8k=true; continue; }

        if(rows==h16.rows && cols==1 && !got16s) { h16.score=m; got16s=true; continue; }
        if(rows==h16.rows && cols==4 && !got16b) { h16.bbox =m; got16b=true; continue; }
        if(rows==h16.rows && cols==10 && !got16k){ h16.kps  =m; got16k=true; continue; }

        if(rows==h32.rows && cols==1 && !got32s) { h32.score=m; got32s=true; continue; }
        if(rows==h32.rows && cols==4 && !got32b) { h32.bbox =m; got32b=true; continue; }
        if(rows==h32.rows && cols==10 && !got32k){ h32.kps  =m; got32k=true; continue; }
    }

    if(got8s && got8b && got8k)  heads.push_back(h8);
    if(got16s && got16b && got16k) heads.push_back(h16);
    if(got32s && got32b && got32k) heads.push_back(h32);

    return !heads.empty();
}

void ScrfdDetectorCvDnn::decode_head(const Head& hd, float score_th, std::vector<FaceBox>& faces)
{
    const int stride = hd.stride;
    const int fm_w = hd.fm_w;

    for(int idx=0; idx<hd.rows; idx++) {
        float s = hd.score.at<float>(idx,0);
        if(s < score_th) continue;

        /* idx -> (y,x,a), a in {0,1} */
        int loc = idx / 2;
        int a   = idx % 2;
        (void)a; // anchor id currently unused (SCRFD 2 anchors)

        int y = loc / fm_w;
        int x = loc % fm_w;

        float cx = (x + 0.5f) * stride;
        float cy = (y + 0.5f) * stride;

        /* bbox: l,t,r,b  —— 关键：必须 * stride，否则框会非常小（你之前鼻子小框就是这个症状） */
        float l = hd.bbox.at<float>(idx,0) * stride;
        float t = hd.bbox.at<float>(idx,1) * stride;
        float r = hd.bbox.at<float>(idx,2) * stride;
        float b = hd.bbox.at<float>(idx,3) * stride;

        FaceBox fb;
        fb.x1 = cx - l;
        fb.y1 = cy - t;
        fb.x2 = cx + r;
        fb.y2 = cy + b;
        fb.score = s;

        /* clamp to 320x320 */
        fb.x1 = std::max(0.0f, std::min(fb.x1, (float)in_w_-1));
        fb.y1 = std::max(0.0f, std::min(fb.y1, (float)in_h_-1));
        fb.x2 = std::max(0.0f, std::min(fb.x2, (float)in_w_-1));
        fb.y2 = std::max(0.0f, std::min(fb.y2, (float)in_h_-1));

        faces.push_back(fb);
    }
}

float ScrfdDetectorCvDnn::iou(const FaceBox& a, const FaceBox& b)
{
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);
    float w = std::max(0.0f, x2-x1);
    float h = std::max(0.0f, y2-y1);
    float inter = w*h;
    float areaA = std::max(0.0f, (a.x2-a.x1)) * std::max(0.0f, (a.y2-a.y1));
    float areaB = std::max(0.0f, (b.x2-b.x1)) * std::max(0.0f, (b.y2-b.y1));
    float uni = areaA + areaB - inter;
    return (uni <= 0.0f) ? 0.0f : (inter / uni);
}

void ScrfdDetectorCvDnn::nms(std::vector<FaceBox>& faces, float nms_th)
{
    std::sort(faces.begin(), faces.end(), [](const FaceBox& a, const FaceBox& b){
        return a.score > b.score;
    });

    std::vector<FaceBox> keep;
    std::vector<char> suppressed(faces.size(), 0);

    for(size_t i=0;i<faces.size();i++){
        if(suppressed[i]) continue;
        keep.push_back(faces[i]);
        for(size_t j=i+1;j<faces.size();j++){
            if(suppressed[j]) continue;
            if(iou(faces[i], faces[j]) > nms_th) suppressed[j] = 1;
        }
    }
    faces.swap(keep);
}

bool ScrfdDetectorCvDnn::detect(const uint8_t* rgb888, int w, int h,
                                std::vector<FaceBox>& out_faces,
                                float score_th, float nms_th)
{
    out_faces.clear();
    if(!inited_ || !rgb888) return false;
    if(w != in_w_ || h != in_h_) {
        /* 你上游已确保送进来就是 320x320，这里直接拒绝避免“动态 shape” */
        return false;
    }

    cv::Mat img(h, w, CV_8UC3, (void*)rgb888);
    cv::Mat blob = cv::dnn::blobFromImage(img, 1.0/128.0, cv::Size(in_w_, in_h_),
                                          cv::Scalar(127.5,127.5,127.5), true, false);

    net_.setInput(blob);

    std::vector<cv::String> outNames = net_.getUnconnectedOutLayersNames();
    std::vector<cv::Mat> outs;
    net_.forward(outs, outNames);

    if(!dumped_) {
        dumped_ = true;
        printf("[ScrfdDetectorCvDnn] ===== ONNX output dump (first forward) =====\n");
        printf("[ScrfdDetectorCvDnn] outNames(%zu):\n", outNames.size());
        for(auto& n : outNames) printf("  - %s\n", n.c_str());
        printf("[ScrfdDetectorCvDnn] outs(%zu):\n", outs.size());
        for(size_t i=0;i<outs.size();i++){
            auto& m = outs[i];
            printf("  [%zu] dims=%d shape=[", i, m.dims);
            for(int d=0; d<m.dims; d++){
                printf("%d%s", m.size[d], (d==m.dims-1)?"]":",");
            }
            printf(" type=%d total=%zu\n", m.type(), (size_t)m.total());
        }
        printf("[ScrfdDetectorCvDnn] ===========================================\n");
        fflush(stdout);
    }

    std::vector<Head> heads;
    if(!collect_heads_320(outs, heads)) return false;

    std::vector<FaceBox> faces;
    for(auto& hd : heads) decode_head(hd, score_th, faces);

    if(faces.empty()) return true;
    nms(faces, nms_th);

    out_faces.swap(faces);
    return true;
}
