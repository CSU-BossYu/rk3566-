#include "opencv_process/face_align.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace face_align {

static inline bool valid_xy(float x, float y) {
    return (x >= -1e6f && x <= 1e6f && y >= -1e6f && y <= 1e6f);
}

// Umeyama similarity transform (2D): dst ≈ s * R * src + t
// 输出 2x3 (float) 给 warpAffine
static bool estimate_similarity_2d_5pts(const float src_xy[10], const float dst_xy[10], cv::Mat& M2x3) {
    constexpr int N = 5;
    cv::Point2f src[N], dst[N];

    for (int i = 0; i < N; ++i) {
        float sx = src_xy[2*i + 0], sy = src_xy[2*i + 1];
        float dx = dst_xy[2*i + 0], dy = dst_xy[2*i + 1];
        if (!valid_xy(sx, sy) || !valid_xy(dx, dy)) return false;
        src[i] = cv::Point2f(sx, sy);
        dst[i] = cv::Point2f(dx, dy);
    }

    // mean
    cv::Point2f mu_s(0,0), mu_d(0,0);
    for (int i = 0; i < N; ++i) { mu_s += src[i]; mu_d += dst[i]; }
    mu_s.x /= N; mu_s.y /= N;
    mu_d.x /= N; mu_d.y /= N;

    // demean & covariance
    double var_s = 0.0;
    cv::Matx22d cov(0,0,0,0); // (1/N) Σ (d_i)(s_i)^T
    for (int i = 0; i < N; ++i) {
        cv::Point2f xs(src[i].x - mu_s.x, src[i].y - mu_s.y);
        cv::Point2f yd(dst[i].x - mu_d.x, dst[i].y - mu_d.y);

        var_s += (double)xs.x * xs.x + (double)xs.y * xs.y;

        cov(0,0) += (double)yd.x * (double)xs.x;
        cov(0,1) += (double)yd.x * (double)xs.y;
        cov(1,0) += (double)yd.y * (double)xs.x;
        cov(1,1) += (double)yd.y * (double)xs.y;
    }
    var_s /= N;
    cov *= (1.0 / N);

    if (var_s < 1e-12) return false;

    cv::Mat cov_m(2, 2, CV_64F);
    cov_m.at<double>(0,0) = cov(0,0);
    cov_m.at<double>(0,1) = cov(0,1);
    cov_m.at<double>(1,0) = cov(1,0);
    cov_m.at<double>(1,1) = cov(1,1);

    cv::SVD svd(cov_m, cv::SVD::FULL_UV);
    cv::Mat U = svd.u;      // 2x2
    cv::Mat Vt = svd.vt;    // 2x2
    cv::Mat W = svd.w;      // 2x1 singular values

    // proper rotation
    double det_uv = cv::determinant(U * Vt);
    cv::Mat D = cv::Mat::eye(2, 2, CV_64F);
    if (det_uv < 0) D.at<double>(1,1) = -1.0;

    cv::Mat R = U * D * Vt;

    // scale
    double s0 = W.at<double>(0,0);
    double s1 = W.at<double>(1,0);
    double scale = (s0 * D.at<double>(0,0) + s1 * D.at<double>(1,1)) / var_s;

    // t = mu_d - scale * R * mu_s
    cv::Mat mu_s_m(2,1,CV_64F);
    mu_s_m.at<double>(0,0) = mu_s.x;
    mu_s_m.at<double>(1,0) = mu_s.y;

    cv::Mat mu_d_m(2,1,CV_64F);
    mu_d_m.at<double>(0,0) = mu_d.x;
    mu_d_m.at<double>(1,0) = mu_d.y;

    cv::Mat t = mu_d_m - scale * (R * mu_s_m);

    M2x3.create(2, 3, CV_32F);
    M2x3.at<float>(0,0) = (float)(scale * R.at<double>(0,0));
    M2x3.at<float>(0,1) = (float)(scale * R.at<double>(0,1));
    M2x3.at<float>(1,0) = (float)(scale * R.at<double>(1,0));
    M2x3.at<float>(1,1) = (float)(scale * R.at<double>(1,1));
    M2x3.at<float>(0,2) = (float)t.at<double>(0,0);
    M2x3.at<float>(1,2) = (float)t.at<double>(1,0);

    return true;
}

bool align_5pts_rgb112(const uint8_t* rgb_in, int w, int h,
                       const float kps_xy[10],
                       uint8_t out112[112 * 112 * 3])
{
    if (!rgb_in || !kps_xy || !out112) return false;
    if (w <= 0 || h <= 0) return false;

    float dst_xy[10];
    for (int i = 0; i < 10; ++i) dst_xy[i] = kArc112Template[i];

    cv::Mat M;
    if (!estimate_similarity_2d_5pts(kps_xy, dst_xy, M)) return false;

    cv::Mat in(h, w, CV_8UC3, const_cast<uint8_t*>(rgb_in)); // RGB
    cv::Mat out(112, 112, CV_8UC3, out112);

    cv::warpAffine(in, out, M, cv::Size(112, 112),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0,0,0));
    return true;
}

} // namespace face_align
