#include "rga_preprocess.h"
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>

RgaPreprocess::Scratch::~Scratch() {
    if (ptr) free(ptr);
    ptr = nullptr;
    bytes = 0;
}

bool RgaPreprocess::Scratch::ensure(size_t need_bytes) {
    if (need_bytes == 0) return false;
    if (ptr && bytes >= need_bytes) return true;

    if (ptr) free(ptr);
    ptr = nullptr;
    bytes = 0;

    void* p = nullptr;
    if (posix_memalign(&p, 64, need_bytes) != 0 || !p) return false;
    ptr = p;
    bytes = need_bytes;
    std::memset(ptr, 0, bytes);
    return true;
}

int RgaPreprocess::align_up_int(int x, int a) {
    return (x + a - 1) / a * a;
}

bool RgaPreprocess::init() {
    last_error_.clear();
    return true;
}

bool RgaPreprocess::check_common_in(const RgaImage& in, int out_w, int out_h) {
    last_error_.clear();
    if (in.width <= 0 || in.height <= 0 || in.stride <= 0) {
        last_error_ = "invalid input size/stride";
        return false;
    }
    if (in.dma_fd < 0 && in.va == nullptr) {
        last_error_ = "input needs valid dma_fd or va";
        return false;
    }
    if (out_w <= 0 || out_h <= 0) {
        last_error_ = "invalid output size";
        return false;
    }
    return true;
}

void RgaPreprocess::wrap_src(const RgaImage& in, int src_wstride_px, int src_hstride_px, rga_buffer_t& src) {
    // ✅你的 im2d 版本就是这两个 API
    if (in.dma_fd >= 0) {
        src = wrapbuffer_fd_t(in.dma_fd,
                              in.width, in.height,
                              src_wstride_px, src_hstride_px,
                              in.format);
    } else {
        src = wrapbuffer_virtualaddr_t(in.va,
                                       in.width, in.height,
                                       src_wstride_px, src_hstride_px,
                                       in.format);
    }
}

bool RgaPreprocess::convert_resize_to_tight(const RgaImage& in,
                                           int src_wstride_px, int src_hstride_px,
                                           int tmp_format, int tmp_bpp,
                                           int out_format, int out_bpp,
                                           int out_w, int out_h,
                                           uint8_t* out_tight) {
    last_error_.clear();
    if (!out_tight) { last_error_ = "out buffer is null"; return false; }

    rga_buffer_t src{};
    wrap_src(in, src_wstride_px, src_hstride_px, src);

    const int tmp_wstride_px = align_up_int(out_w, 16);
    const int tmp_hstride_px = out_h;

    size_t tmp_bytes = 0;
    if (tmp_format == RK_FORMAT_YCbCr_420_SP) tmp_bytes = (size_t)tmp_wstride_px * (size_t)tmp_hstride_px * 3 / 2;
    else tmp_bytes = (size_t)tmp_wstride_px * (size_t)tmp_hstride_px * (size_t)tmp_bpp;

    if (!tmp_.ensure(tmp_bytes)) { last_error_ = "alloc tmp failed"; return false; }

    rga_buffer_t tmp = wrapbuffer_virtualaddr_t(tmp_.ptr,
                                               out_w, out_h,
                                               tmp_wstride_px, tmp_hstride_px,
                                               tmp_format);

    const int dst_wstride_px = align_up_int(out_w, 16);
    const int dst_hstride_px = out_h;
    const int dst_bytes_per_line = dst_wstride_px * out_bpp;
    const size_t dst_pad_bytes = (size_t)dst_bytes_per_line * (size_t)dst_hstride_px;

    if (!dst_pad_.ensure(dst_pad_bytes)) { last_error_ = "alloc dst_pad failed"; return false; }

    rga_buffer_t dst = wrapbuffer_virtualaddr_t(dst_pad_.ptr,
                                               out_w, out_h,
                                               dst_wstride_px, dst_hstride_px,
                                               out_format);

    rga_buffer_t pat{};
    im_rect none{};

    IM_STATUS chk1 = imcheck_t(src, tmp, pat, none, none, none, 0);
    if (chk1 != IM_STATUS_SUCCESS && chk1 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcheck(src->tmp) failed: ") + imStrError(chk1);
        return false;
    }
    IM_STATUS chk2 = imcheck_t(tmp, dst, pat, none, none, none, 0);
    if (chk2 != IM_STATUS_SUCCESS && chk2 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcheck(tmp->dst) failed: ") + imStrError(chk2);
        return false;
    }

    // 你的版本 imresize_t 只用 fx/fy；这里传 0/0 表示自动按 dst 尺寸
    IM_STATUS s1 = imresize_t(src, tmp, 0, 0, INTER_LINEAR, 1);
    if (s1 != IM_STATUS_SUCCESS && s1 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imresize failed: ") + imStrError(s1);
        return false;
    }

    IM_STATUS s2 = imcvtcolor_t(tmp, dst, tmp_format, out_format, IM_COLOR_SPACE_DEFAULT, 1);
    if (s2 != IM_STATUS_SUCCESS && s2 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcvtcolor failed: ") + imStrError(s2);
        return false;
    }

    const uint8_t* src_pad = (const uint8_t*)dst_pad_.ptr;
    const size_t tight_row_bytes = (size_t)out_w * (size_t)out_bpp;
    for (int y = 0; y < out_h; ++y) {
        std::memcpy(out_tight + (size_t)y * tight_row_bytes,
                    src_pad  + (size_t)y * (size_t)dst_bytes_per_line,
                    tight_row_bytes);
    }
    return true;
}

bool RgaPreprocess::convert_crop_resize_to_tight(const RgaImage& in,
    int src_wstride_px, int src_hstride_px,
    int crop_x, int crop_y, int crop_w, int crop_h,
    int tmp_format, int tmp_bpp,
    int out_format, int out_bpp,
    int out_w, int out_h,
    uint8_t* out_tight)
{
    last_error_.clear();
    if (!out_tight) { last_error_ = "out buffer is null"; return false; }

    // clamp crop
    if (crop_x < 0) crop_x = 0;
    if (crop_y < 0) crop_y = 0;
    if (crop_x + crop_w > in.width)  crop_w = in.width  - crop_x;
    if (crop_y + crop_h > in.height) crop_h = in.height - crop_y;
    if (crop_w <= 0 || crop_h <= 0) { last_error_ = "crop rect out of range"; return false; }

    rga_buffer_t src{};
    wrap_src(in, src_wstride_px, src_hstride_px, src);

    // crop buffer (virtual)
    const int crop_wstride_px = align_up_int(crop_w, 16);
    const int crop_hstride_px = crop_h;

    size_t crop_bytes = 0;
    if (tmp_format == RK_FORMAT_YCbCr_420_SP) crop_bytes = (size_t)crop_wstride_px * (size_t)crop_hstride_px * 3 / 2;
    else crop_bytes = (size_t)crop_wstride_px * (size_t)crop_hstride_px * (size_t)tmp_bpp;

    std::vector<uint8_t> crop_buf(crop_bytes);

    rga_buffer_t crop = wrapbuffer_virtualaddr_t(
        crop_buf.data(),
        crop_w, crop_h,
        crop_wstride_px, crop_hstride_px,
        tmp_format
    );

    // tmp buffer (resize output, still tmp_format)
    const int tmp_wstride_px = align_up_int(out_w, 16);
    const int tmp_hstride_px = out_h;

    size_t tmp_bytes = 0;
    if (tmp_format == RK_FORMAT_YCbCr_420_SP) tmp_bytes = (size_t)tmp_wstride_px * (size_t)tmp_hstride_px * 3 / 2;
    else tmp_bytes = (size_t)tmp_wstride_px * (size_t)tmp_hstride_px * (size_t)tmp_bpp;

    if (!tmp_.ensure(tmp_bytes)) { last_error_ = "alloc tmp failed"; return false; }

    rga_buffer_t tmp = wrapbuffer_virtualaddr_t(
        tmp_.ptr,
        out_w, out_h,
        tmp_wstride_px, tmp_hstride_px,
        tmp_format
    );

    // dst padded
    const int dst_wstride_px = align_up_int(out_w, 16);
    const int dst_hstride_px = out_h;
    const int dst_bytes_per_line = dst_wstride_px * out_bpp;
    const size_t dst_pad_bytes = (size_t)dst_bytes_per_line * (size_t)dst_hstride_px;

    if (!dst_pad_.ensure(dst_pad_bytes)) { last_error_ = "alloc dst_pad failed"; return false; }

    rga_buffer_t dst = wrapbuffer_virtualaddr_t(
        dst_pad_.ptr,
        out_w, out_h,
        dst_wstride_px, dst_hstride_px,
        out_format
    );

    // check + crop + resize + cvtcolor
    rga_buffer_t pat{};
    im_rect none{};

    IM_STATUS chk0 = imcheck_t(src, crop, pat, none, none, none, 0);
    if (chk0 != IM_STATUS_SUCCESS && chk0 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcheck(src->crop) failed: ") + imStrError(chk0);
        return false;
    }
    IM_STATUS chk1 = imcheck_t(crop, tmp, pat, none, none, none, 0);
    if (chk1 != IM_STATUS_SUCCESS && chk1 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcheck(crop->tmp) failed: ") + imStrError(chk1);
        return false;
    }
    IM_STATUS chk2 = imcheck_t(tmp, dst, pat, none, none, none, 0);
    if (chk2 != IM_STATUS_SUCCESS && chk2 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcheck(tmp->dst) failed: ") + imStrError(chk2);
        return false;
    }

    im_rect rect{};
    rect.x = crop_x;
    rect.y = crop_y;
    rect.width  = crop_w;
    rect.height = crop_h;

    IM_STATUS s0 = imcrop_t(src, crop, rect, 1);
    if (s0 != IM_STATUS_SUCCESS && s0 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcrop failed: ") + imStrError(s0);
        return false;
    }

    const double fx = (double)out_w / (double)crop_w;
    const double fy = (double)out_h / (double)crop_h;

    IM_STATUS s1 = imresize_t(crop, tmp, fx, fy, INTER_LINEAR, 1);
    if (s1 != IM_STATUS_SUCCESS && s1 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imresize failed: ") + imStrError(s1);
        return false;
    }

    IM_STATUS s2 = imcvtcolor_t(tmp, dst, tmp_format, out_format, IM_COLOR_SPACE_DEFAULT, 1);
    if (s2 != IM_STATUS_SUCCESS && s2 != IM_STATUS_NOERROR) {
        last_error_ = std::string("imcvtcolor failed: ") + imStrError(s2);
        return false;
    }

    // padded -> tight
    const uint8_t* src_pad = (const uint8_t*)dst_pad_.ptr;
    const size_t tight_row_bytes = (size_t)out_w * (size_t)out_bpp;

    for (int y = 0; y < out_h; ++y) {
        std::memcpy(out_tight + (size_t)y * tight_row_bytes,
                    src_pad  + (size_t)y * (size_t)dst_bytes_per_line,
                    tight_row_bytes);
    }
    return true;
}

/* ---------------- NV12 -> RGB888 ---------------- */
bool RgaPreprocess::nv12_to_rgb_resize(const RgaImage& in_nv12, uint8_t* out_rgb, int out_w, int out_h) {
    if (!check_common_in(in_nv12, out_w, out_h)) return false;
    if (in_nv12.format != RK_FORMAT_YCbCr_420_SP) { last_error_ = "input must be RK_FORMAT_YCbCr_420_SP (NV12)"; return false; }
    const int src_wstride_px = in_nv12.stride;
    const int src_hstride_px = in_nv12.height;
    return convert_resize_to_tight(in_nv12, src_wstride_px, src_hstride_px,
                                   RK_FORMAT_YCbCr_420_SP, 0,
                                   RK_FORMAT_RGB_888, 3,
                                   out_w, out_h, out_rgb);
}
bool RgaPreprocess::nv12_to_rgb_resize(const RgaImage& in_nv12, std::vector<uint8_t>& out_rgb, int out_w, int out_h) {
    out_rgb.resize((size_t)out_w * (size_t)out_h * 3);
    return nv12_to_rgb_resize(in_nv12, out_rgb.data(), out_w, out_h);
}

/* ---------------- NV12 -> BGR888 ---------------- */
bool RgaPreprocess::nv12_to_bgr_resize(const RgaImage& in_nv12, uint8_t* out_bgr, int out_w, int out_h) {
    if (!check_common_in(in_nv12, out_w, out_h)) return false;
    if (in_nv12.format != RK_FORMAT_YCbCr_420_SP) { last_error_ = "input must be RK_FORMAT_YCbCr_420_SP (NV12)"; return false; }
    const int src_wstride_px = in_nv12.stride;
    const int src_hstride_px = in_nv12.height;
    return convert_resize_to_tight(in_nv12, src_wstride_px, src_hstride_px,
                                   RK_FORMAT_YCbCr_420_SP, 0,
                                   RK_FORMAT_BGR_888, 3,
                                   out_w, out_h, out_bgr);
}
bool RgaPreprocess::nv12_to_bgr_resize(const RgaImage& in_nv12, std::vector<uint8_t>& out_bgr, int out_w, int out_h) {
    out_bgr.resize((size_t)out_w * (size_t)out_h * 3);
    return nv12_to_bgr_resize(in_nv12, out_bgr.data(), out_w, out_h);
}

/* ---------------- YUYV422 -> RGB888 ---------------- */
bool RgaPreprocess::yuyv_to_rgb_resize(const RgaImage& in_yuyv, uint8_t* out_rgb, int out_w, int out_h) {
    if (!check_common_in(in_yuyv, out_w, out_h)) return false;
    if (in_yuyv.format != RK_FORMAT_YUYV_422) { last_error_ = "input must be RK_FORMAT_YUYV_422"; return false; }
    if ((in_yuyv.stride % 2) != 0) { last_error_ = "YUYV stride(bytes) must be even"; return false; }

    const int src_wstride_px = in_yuyv.stride / 2;
    const int src_hstride_px = in_yuyv.height;

    return convert_resize_to_tight(in_yuyv, src_wstride_px, src_hstride_px,
                                   RK_FORMAT_YUYV_422, 2,
                                   RK_FORMAT_RGB_888, 3,
                                   out_w, out_h, out_rgb);
}
bool RgaPreprocess::yuyv_to_rgb_resize(const RgaImage& in_yuyv, std::vector<uint8_t>& out_rgb, int out_w, int out_h) {
    out_rgb.resize((size_t)out_w * (size_t)out_h * 3);
    return yuyv_to_rgb_resize(in_yuyv, out_rgb.data(), out_w, out_h);
}

/* ---------------- YUYV422 -> BGRA8888 ---------------- */
bool RgaPreprocess::yuyv_to_bgra_resize(const RgaImage& in_yuyv, uint8_t* out_bgra, int out_w, int out_h) {
    if (!check_common_in(in_yuyv, out_w, out_h)) return false;
    if (in_yuyv.format != RK_FORMAT_YUYV_422) { last_error_ = "input must be RK_FORMAT_YUYV_422"; return false; }
    if ((in_yuyv.stride % 2) != 0) { last_error_ = "YUYV stride(bytes) must be even"; return false; }

    const int src_wstride_px = in_yuyv.stride / 2;
    const int src_hstride_px = in_yuyv.height;

    return convert_resize_to_tight(in_yuyv, src_wstride_px, src_hstride_px,
                                   RK_FORMAT_YUYV_422, 2,
                                   RK_FORMAT_BGRA_8888, 4,
                                   out_w, out_h, out_bgra);
}
bool RgaPreprocess::yuyv_to_bgra_resize(const RgaImage& in_yuyv, std::vector<uint8_t>& out_bgra, int out_w, int out_h) {
    out_bgra.resize((size_t)out_w * (size_t)out_h * 4);
    return yuyv_to_bgra_resize(in_yuyv, out_bgra.data(), out_w, out_h);
}

/* ---------------- YUYV crop -> BGRA8888 (UI) ---------------- */
bool RgaPreprocess::yuyv_crop_to_bgra_resize(const RgaImage& in_yuyv,
                                             int crop_x, int crop_y, int crop_w, int crop_h,
                                             uint8_t* out_bgra, int out_w, int out_h) {
    if (!check_common_in(in_yuyv, out_w, out_h)) return false;
    if (in_yuyv.format != RK_FORMAT_YUYV_422) { last_error_ = "input must be RK_FORMAT_YUYV_422"; return false; }
    if ((in_yuyv.stride % 2) != 0) { last_error_ = "YUYV stride(bytes) must be even"; return false; }

    const int src_wstride_px = in_yuyv.stride / 2;
    const int src_hstride_px = in_yuyv.height;

    return convert_crop_resize_to_tight(in_yuyv,
                                        src_wstride_px, src_hstride_px,
                                        crop_x, crop_y, crop_w, crop_h,
                                        RK_FORMAT_YUYV_422, 2,
                                        RK_FORMAT_BGRA_8888, 4,
                                        out_w, out_h,
                                        out_bgra);
}

/* ---------------- ★Commit C：YUYV crop -> RGB888 (DET) ---------------- */
bool RgaPreprocess::yuyv_crop_to_rgb_resize(const RgaImage& in_yuyv,
                                            int crop_x, int crop_y, int crop_w, int crop_h,
                                            uint8_t* out_rgb, int out_w, int out_h) {
    if (!check_common_in(in_yuyv, out_w, out_h)) return false;
    if (in_yuyv.format != RK_FORMAT_YUYV_422) { last_error_ = "input must be RK_FORMAT_YUYV_422"; return false; }
    if ((in_yuyv.stride % 2) != 0) { last_error_ = "YUYV stride(bytes) must be even"; return false; }

    const int src_wstride_px = in_yuyv.stride / 2;
    const int src_hstride_px = in_yuyv.height;

    return convert_crop_resize_to_tight(in_yuyv,
                                        src_wstride_px, src_hstride_px,
                                        crop_x, crop_y, crop_w, crop_h,
                                        RK_FORMAT_YUYV_422, 2,
                                        RK_FORMAT_RGB_888, 3,
                                        out_w, out_h,
                                        out_rgb);
}

bool RgaPreprocess::yuyv_crop_to_rgb_resize(const RgaImage& in_yuyv,
                                            int crop_x, int crop_y, int crop_w, int crop_h,
                                            std::vector<uint8_t>& out_rgb, int out_w, int out_h) {
    out_rgb.resize((size_t)out_w * (size_t)out_h * 3);
    return yuyv_crop_to_rgb_resize(in_yuyv, crop_x, crop_y, crop_w, crop_h,
                                   out_rgb.data(), out_w, out_h);
}
