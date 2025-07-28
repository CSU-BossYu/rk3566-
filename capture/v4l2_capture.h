#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <pthread.h>

// V4L2 capture helper for this demo.
// IMPORTANT: To avoid crashes caused by C++ exception unwinding on some Android vendor stacks,
// this class is designed to NEVER throw exceptions. Always check ok()/lastError().
class V4L2Capture {
public:
    struct Frame {
        const uint8_t* data = nullptr;
        size_t bytes_used = 0;
        int width = 0;
        int height = 0;
        int stride = 0;          // bytesperline
        uint32_t pixfmt = 0;
        int index = -1;
        uint64_t ts_us = 0;

        // Exported dmabuf fd (if EXPBUF supported). If not, = -1.
        int dma_fd = -1;

        Frame() = default;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&&) noexcept;
        Frame& operator=(Frame&&) noexcept;
        ~Frame();

    private:
        friend class V4L2Capture;
        V4L2Capture* owner = nullptr;
    };

    // 新增：RawFrame（不带 data/va，不自动 requeue），配合 FrameRef（引用计数）使用
    struct RawFrame {
        int index = -1;
        int dma_fd = -1;
        size_t bytes_used = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        uint32_t pixfmt = 0;
        uint64_t ts_us = 0;
    };

    // ctor never throws; check ok() after construction.
    V4L2Capture(const std::string& dev, int width, int height, uint32_t pixfmt, int reqBufs = 4);
    ~V4L2Capture();

    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    bool ok() const { return ok_; }
    const std::string& lastError() const { return last_error_; }

    // grab one frame; returns {index=-1} on failure/timeout. lastError() contains details if any.
    // 旧接口：返回 Frame（RAII，析构自动 qbuf）
    Frame grab(int timeout_ms);

    // 新增接口：dequeue 一帧（不自动 qbuf），由调用者在合适时机 requeue(index)
    // 返回 false 表示超时或失败（失败可看 lastError）
    bool dequeue(int timeout_ms, RawFrame& out);

    // 新增接口：归还 buffer（线程安全）
    void requeue(int index);

    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    uint32_t pixfmt() const { return pixfmt_; }

private:
    struct Buffer {
        void*  start0 = nullptr;
        size_t length0 = 0;
        uint32_t offset0 = 0; // for MMAP
        int    dma_fd0 = -1;  // for EXPBUF
    };

    int fd_ = -1;

    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    uint32_t pixfmt_ = 0;

    int num_bufs_ = 0;

    // Chosen buffer type:
    // - V4L2_BUF_TYPE_VIDEO_CAPTURE (single-plane)
    // - V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE (multi-plane)
    uint32_t buf_type_ = 0;

    // For MPLANE (we only support 1 plane in this demo)
    int num_planes_ = 1;

    std::vector<Buffer> bufs_;
    bool streaming_ = false;

    bool ok_ = false;
    std::string last_error_;

    // 新增：保护 qbuf/requeue 的锁（允许跨线程 requeue）
    pthread_mutex_t q_mtx_{};

    void setError(const std::string& msg) { last_error_ = msg; }
    void cleanupOnFail();

    void qbuf(int index); // never throws
    int  dqbuf(int timeout_ms, size_t& bytes_used, uint64_t& ts_us); // returns index or negative
};
