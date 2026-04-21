#include "v4l2_capture.h"

// V4L2 核心头文件
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

// C++ 基础库
#include <cstring>
#include <string>
#include <iostream>

static std::string errno_str(const std::string& what) {
    return what + " errno=" + std::to_string(errno) + " (" + std::string(strerror(errno)) + ")";
}

static uint64_t tv_to_us(const timeval& tv) {
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static uint64_t now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int default_stride_bytes(int width, uint32_t pixfmt) {
    switch (pixfmt) {
        case V4L2_PIX_FMT_YUYV: return width * 2;
        case V4L2_PIX_FMT_UYVY: return width * 2;
        case V4L2_PIX_FMT_RGB24: return width * 3;
        case V4L2_PIX_FMT_BGR24: return width * 3;
        case V4L2_PIX_FMT_NV12: return width;
        default: return 0;
    }
}

V4L2Capture::Frame::Frame(Frame&& other) noexcept { *this = std::move(other); }

V4L2Capture::Frame& V4L2Capture::Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        if (owner && index >= 0) owner->qbuf(index);

        owner = other.owner; other.owner = nullptr;
        data = other.data; other.data = nullptr;
        bytes_used = other.bytes_used; other.bytes_used = 0;
        width = other.width; other.width = 0;
        height = other.height; other.height = 0;
        stride = other.stride; other.stride = 0;
        pixfmt = other.pixfmt; other.pixfmt = 0;
        index = other.index; other.index = -1;
        ts_us = other.ts_us; other.ts_us = 0;
        dma_fd = other.dma_fd; other.dma_fd = -1;
    }
    return *this;
}

V4L2Capture::Frame::~Frame() {
    if (owner && index >= 0) owner->qbuf(index);
}

void V4L2Capture::cleanupOnFail() {
    if (fd_ >= 0) {
        if (streaming_) {
            v4l2_buf_type type = (v4l2_buf_type)buf_type_;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
        }

        for (auto& b : bufs_) {
            if (b.start0 && b.length0) munmap(b.start0, b.length0);
            b.start0 = nullptr;
            b.length0 = 0;

            if (b.dma_fd0 >= 0) close(b.dma_fd0);
            b.dma_fd0 = -1;
        }
        bufs_.clear();

        close(fd_);
        fd_ = -1;
    }

    streaming_ = false;
    ok_ = false;
}

void V4L2Capture::try_set_fps_(int fps) {
    if (fps <= 0) return;

    v4l2_streamparm sp{};
    sp.type = buf_type_;
    if (xioctl(fd_, VIDIOC_G_PARM, &sp) < 0) {
        // 有的驱动不支持，别当致命错误
        std::fprintf(stderr, "[CAM] VIDIOC_G_PARM not supported\n");
        return;
    }

    if (!(sp.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        std::fprintf(stderr, "[CAM] TIMEPERFRAME not supported by driver\n");
        return;
    }

    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = (uint32_t)fps;

    if (xioctl(fd_, VIDIOC_S_PARM, &sp) < 0) {
        std::fprintf(stderr, "[CAM] VIDIOC_S_PARM set fps=%d failed: %s\n",
                     fps, strerror(errno));
        return;
    }

    // 读回确认实际生效值
    v4l2_streamparm sp2{};
    sp2.type = buf_type_;
    if (xioctl(fd_, VIDIOC_G_PARM, &sp2) == 0) {
        auto n = sp2.parm.capture.timeperframe.numerator;
        auto d = sp2.parm.capture.timeperframe.denominator;
        if (n == 0) n = 1;
        double real_fps = (double)d / (double)n;
        std::fprintf(stderr, "[CAM] FPS request=%d, applied timeperframe=%u/%u (%.2f fps)\n",
                     fps, n, d, real_fps);
    }
}

V4L2Capture::V4L2Capture(const std::string& dev, int width, int height, uint32_t pixfmt, int reqBufs, int fps)
    : width_(width), height_(height), pixfmt_(pixfmt) {

    ok_ = false;
    last_error_.clear();

    pthread_mutex_init(&q_mtx_, nullptr);

    if (reqBufs < 3) reqBufs = 3;
    if (reqBufs > 16) reqBufs = 16;

    fd_ = open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        setError(errno_str("open v4l2 failed: " + dev));
        cleanupOnFail();
        return;
    }

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        setError(errno_str("VIDIOC_QUERYCAP failed"));
        cleanupOnFail();
        return;
    }

    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;

    const bool has_stream = (caps & V4L2_CAP_STREAMING) != 0;
    if (!has_stream) {
        setError("device does not support streaming");
        cleanupOnFail();
        return;
    }

    const bool has_capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
    const bool has_capture_mplane = (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
    if (!has_capture && !has_capture_mplane) {
        setError("device does not support VIDEO_CAPTURE");
        cleanupOnFail();
        return;
    }

    buf_type_ = has_capture ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = (uint32_t)width_;
        fmt.fmt.pix.height = (uint32_t)height_;
        fmt.fmt.pix.pixelformat = pixfmt_;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            setError(errno_str("VIDIOC_S_FMT (CAPTURE) failed"));
            cleanupOnFail();
            return;
        }

        width_ = (int)fmt.fmt.pix.width;
        height_ = (int)fmt.fmt.pix.height;
        pixfmt_ = fmt.fmt.pix.pixelformat;

        stride_ = (int)fmt.fmt.pix.bytesperline;
        if (stride_ == 0) stride_ = default_stride_bytes(width_, pixfmt_);
    } else {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width = (uint32_t)width_;
        fmt.fmt.pix_mp.height = (uint32_t)height_;
        fmt.fmt.pix_mp.pixelformat = pixfmt_;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            setError(errno_str("VIDIOC_S_FMT (MPLANE) failed"));
            cleanupOnFail();
            return;
        }

        width_ = (int)fmt.fmt.pix_mp.width;
        height_ = (int)fmt.fmt.pix_mp.height;
        pixfmt_ = fmt.fmt.pix_mp.pixelformat;

        num_planes_ = (int)fmt.fmt.pix_mp.num_planes;
        if (num_planes_ != 1) {
            setError("MPLANE num_planes != 1 (this demo expects 1 plane)");
            cleanupOnFail();
            return;
        }

        stride_ = (int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (stride_ == 0) stride_ = default_stride_bytes(width_, pixfmt_);
    }

    // ✅Commit J.2.2：在申请 buffer 之前设置 FPS（驱动通常要求此顺序）
    try_set_fps_(fps);

    v4l2_requestbuffers req{};
    req.count = (uint32_t)reqBufs;
    req.type = buf_type_;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        setError(errno_str("VIDIOC_REQBUFS failed"));
        cleanupOnFail();
        return;
    }
    if (req.count < 2) {
        setError("VIDIOC_REQBUFS returned too few buffers");
        cleanupOnFail();
        return;
    }

    num_bufs_ = (int)req.count;
    bufs_.resize((size_t)num_bufs_);

    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        for (uint32_t i = 0; i < (uint32_t)num_bufs_; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                setError(errno_str("VIDIOC_QUERYBUF (CAPTURE) failed"));
                cleanupOnFail();
                return;
            }

            bufs_[i].length0 = buf.length;
            bufs_[i].offset0 = buf.m.offset;

            void* start0 = mmap(nullptr, bufs_[i].length0,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd_, bufs_[i].offset0);
            if (start0 == MAP_FAILED) {
                setError(errno_str("mmap failed"));
                cleanupOnFail();
                return;
            }
            bufs_[i].start0 = start0;

            v4l2_exportbuffer exp{};
            exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            exp.index = i;
            exp.plane = 0;
            exp.flags = O_CLOEXEC;
            if (xioctl(fd_, VIDIOC_EXPBUF, &exp) == 0) bufs_[i].dma_fd0 = exp.fd;
            else bufs_[i].dma_fd0 = -1;

            qbuf((int)i);
        }
    } else {
        for (uint32_t i = 0; i < (uint32_t)num_bufs_; ++i) {
            v4l2_buffer buf{};
            v4l2_plane planes[VIDEO_MAX_PLANES]{};

            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            buf.m.planes = planes;
            buf.length = (uint32_t)num_planes_;

            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                setError(errno_str("VIDIOC_QUERYBUF (MPLANE) failed"));
                cleanupOnFail();
                return;
            }

            bufs_[i].length0 = buf.m.planes[0].length;
            bufs_[i].offset0 = buf.m.planes[0].m.mem_offset;

            void* start0 = mmap(nullptr, bufs_[i].length0,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd_, bufs_[i].offset0);
            if (start0 == MAP_FAILED) {
                setError(errno_str("mmap failed"));
                cleanupOnFail();
                return;
            }
            bufs_[i].start0 = start0;

            v4l2_exportbuffer exp{};
            exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            exp.index = i;
            exp.plane = 0;
            exp.flags = O_CLOEXEC;
            if (xioctl(fd_, VIDIOC_EXPBUF, &exp) == 0) bufs_[i].dma_fd0 = exp.fd;
            else bufs_[i].dma_fd0 = -1;

            qbuf((int)i);
        }
    }

    v4l2_buf_type type = (v4l2_buf_type)buf_type_;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        setError(errno_str("VIDIOC_STREAMON failed"));
        cleanupOnFail();
        return;
    }

    streaming_ = true;
    ok_ = true;
}

V4L2Capture::~V4L2Capture() {
    cleanupOnFail();
    pthread_mutex_destroy(&q_mtx_);
}

void V4L2Capture::qbuf(int index) {
    if (fd_ < 0) return;
    if (index < 0 || index >= (int)bufs_.size()) return;

    pthread_mutex_lock(&q_mtx_);

    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (uint32_t)index;

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            setError(errno_str("VIDIOC_QBUF (CAPTURE) failed"));
            pthread_mutex_unlock(&q_mtx_);
            return;
        }
    } else {
        v4l2_buffer buf{};
        v4l2_plane planes[VIDEO_MAX_PLANES]{};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (uint32_t)index;
        buf.m.planes = planes;
        buf.length = (uint32_t)num_planes_;

        planes[0].length = (uint32_t)bufs_[index].length0;
        planes[0].m.mem_offset = bufs_[index].offset0;
        planes[0].bytesused = 0;

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            setError(errno_str("VIDIOC_QBUF (MPLANE) failed"));
            pthread_mutex_unlock(&q_mtx_);
            return;
        }
    }

    pthread_mutex_unlock(&q_mtx_);
}

void V4L2Capture::requeue(int index) { qbuf(index); }

int V4L2Capture::dqbuf(int timeout_ms, size_t& bytes_used, uint64_t& ts_us) {
    if (fd_ < 0) { setError("dqbuf on invalid fd"); return -1; }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);

    timeval tv{};
    timeval* tv_ptr = (timeout_ms < 0) ? nullptr : &tv;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }

    int r = select(fd_ + 1, &fds, nullptr, nullptr, tv_ptr);
    if (r == 0) return -1;
    if (r < 0) { setError(errno_str("select failed")); return -1; }

    if (buf_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return -2;
            setError(errno_str("VIDIOC_DQBUF (CAPTURE) failed"));
            return -1;
        }

        bytes_used = (size_t)buf.bytesused;
        ts_us = (buf.timestamp.tv_sec || buf.timestamp.tv_usec) ? tv_to_us(buf.timestamp) : now_us();
        return (int)buf.index;
    } else {
        v4l2_buffer buf{};
        v4l2_plane planes[VIDEO_MAX_PLANES]{};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = (uint32_t)num_planes_;

        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return -2;
            setError(errno_str("VIDIOC_DQBUF (MPLANE) failed"));
            return -1;
        }

        bytes_used = (size_t)buf.m.planes[0].bytesused;
        ts_us = (buf.timestamp.tv_sec || buf.timestamp.tv_usec) ? tv_to_us(buf.timestamp) : now_us();
        return (int)buf.index;
    }
}

V4L2Capture::Frame V4L2Capture::grab(int timeout_ms) {
    Frame f;
    if (!ok_ || fd_ < 0) { setError("V4L2Capture not initialized"); return f; }

    size_t bytesUsed = 0;
    uint64_t ts = 0;
    int idx = dqbuf(timeout_ms, bytesUsed, ts);
    if (idx < 0) return f;
    if (idx >= (int)bufs_.size()) { setError("invalid dqbuf index"); return f; }

    f.owner = this;
    f.index = idx;
    f.data = static_cast<const uint8_t*>(bufs_[idx].start0);
    f.bytes_used = bytesUsed;
    f.width = width_;
    f.height = height_;
    f.stride = stride_;
    f.pixfmt = pixfmt_;
    f.ts_us = ts;
    f.dma_fd = bufs_[idx].dma_fd0;
    return f;
}

bool V4L2Capture::dequeue(int timeout_ms, RawFrame& out) {
    out = RawFrame{};
    if (!ok_ || fd_ < 0) { setError("V4L2Capture not initialized"); return false; }

    size_t bytesUsed = 0;
    uint64_t ts = 0;
    int idx = dqbuf(timeout_ms, bytesUsed, ts);
    if (idx < 0) return false;
    if (idx >= (int)bufs_.size()) { setError("invalid dqbuf index"); return false; }

    out.index = idx;
    out.dma_fd = bufs_[idx].dma_fd0;
    out.bytes_used = bytesUsed;
    out.width = width_;
    out.height = height_;
    out.stride = stride_;
    out.pixfmt = pixfmt_;
    out.ts_us = ts;
    return true;
}
