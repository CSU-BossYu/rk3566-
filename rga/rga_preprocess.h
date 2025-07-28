#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Rockchip RGA im2d
#include "rga/include/im2d.h"
#include "rga/include/rga.h"

struct RgaImage {
    int width = 0;
    int height = 0;
    int stride = 0;     // bytesperline (for YUYV: bytes)
    int format = 0;     // RK_FORMAT_*
    int dma_fd = -1;    // preferred
    void* va = nullptr; // optional
};

class RgaPreprocess {
public:
    bool init();
    const std::string& lastError() const { return last_error_; }

    // NV12 -> RGB/BGR
    bool nv12_to_rgb_resize(const RgaImage& in_nv12, uint8_t* out_rgb, int out_w, int out_h);
    bool nv12_to_rgb_resize(const RgaImage& in_nv12, std::vector<uint8_t>& out_rgb, int out_w, int out_h);

    bool nv12_to_bgr_resize(const RgaImage& in_nv12, uint8_t* out_bgr, int out_w, int out_h);
    bool nv12_to_bgr_resize(const RgaImage& in_nv12, std::vector<uint8_t>& out_bgr, int out_w, int out_h);

    // YUYV -> RGB/BGRA
    bool yuyv_to_rgb_resize(const RgaImage& in_yuyv, uint8_t* out_rgb, int out_w, int out_h);
    bool yuyv_to_rgb_resize(const RgaImage& in_yuyv, std::vector<uint8_t>& out_rgb, int out_w, int out_h);

    bool yuyv_to_bgra_resize(const RgaImage& in_yuyv, uint8_t* out_bgra, int out_w, int out_h);
    bool yuyv_to_bgra_resize(const RgaImage& in_yuyv, std::vector<uint8_t>& out_bgra, int out_w, int out_h);

    // YUYV crop -> resize -> BGRA (UI)
    bool yuyv_crop_to_bgra_resize(const RgaImage& in_yuyv,
                                  int crop_x, int crop_y, int crop_w, int crop_h,
                                  uint8_t* out_bgra, int out_w, int out_h);

    // ★Commit C 需要：YUYV crop -> resize -> RGB (DET 输入)
    bool yuyv_crop_to_rgb_resize(const RgaImage& in_yuyv,
                                 int crop_x, int crop_y, int crop_w, int crop_h,
                                 uint8_t* out_rgb, int out_w, int out_h);
    bool yuyv_crop_to_rgb_resize(const RgaImage& in_yuyv,
                                 int crop_x, int crop_y, int crop_w, int crop_h,
                                 std::vector<uint8_t>& out_rgb, int out_w, int out_h);

private:
    struct Scratch {
        void*  ptr = nullptr;
        size_t bytes = 0;
        ~Scratch();
        bool ensure(size_t need_bytes);
    };

    bool check_common_in(const RgaImage& in, int out_w, int out_h);

    void wrap_src(const RgaImage& in, int src_wstride_px, int src_hstride_px, rga_buffer_t& src);

    bool convert_resize_to_tight(const RgaImage& in,
                                int src_wstride_px, int src_hstride_px,
                                int tmp_format, int tmp_bpp,
                                int out_format, int out_bpp,
                                int out_w, int out_h,
                                uint8_t* out_tight);

    bool convert_crop_resize_to_tight(const RgaImage& in,
                                     int src_wstride_px, int src_hstride_px,
                                     int crop_x, int crop_y, int crop_w, int crop_h,
                                     int tmp_format, int tmp_bpp,
                                     int out_format, int out_bpp,
                                     int out_w, int out_h,
                                     uint8_t* out_tight);

    static int align_up_int(int x, int a);

private:
    std::string last_error_;
    Scratch tmp_;
    Scratch dst_pad_;
};
