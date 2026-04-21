#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <pthread.h>

class V4L2Capture {
public:
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

    struct Frame {
        V4L2Capture* owner = nullptr;
        const uint8_t* data = nullptr;
        size_t bytes_used = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        uint32_t pixfmt = 0;
        int index = -1;
        uint64_t ts_us = 0;
        int dma_fd = -1;

        Frame() = default;
        Frame(Frame&& other) noexcept;
        Frame& operator=(Frame&& other) noexcept;
        ~Frame();

        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
    };

public:
    // fps<=0 表示不设置，走驱动默认
    V4L2Capture(const std::string& dev,
                int width,
                int height,
                uint32_t pixfmt,
                int reqBufs = 4,
                int fps = 0);

    ~V4L2Capture();

    bool ok() const { return ok_; }
    const std::string& lastError() const { return last_error_; }

    Frame grab(int timeout_ms);
    bool dequeue(int timeout_ms, RawFrame& out);
    void requeue(int index);

private:
    void qbuf(int index);
    int dqbuf(int timeout_ms, size_t& bytes_used, uint64_t& ts_us);

    void cleanupOnFail();
    void setError(const std::string& s) { last_error_ = s; }

    void try_set_fps_(int fps);

private:
    struct Buffer {
        void* start0 = nullptr;
        size_t length0 = 0;
        size_t offset0 = 0;
        int dma_fd0 = -1;
    };

    int fd_ = -1;
    bool ok_ = false;
    bool streaming_ = false;

    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    uint32_t pixfmt_ = 0;

    int buf_type_ = 0;
    int num_planes_ = 0;
    int num_bufs_ = 0;
    std::vector<Buffer> bufs_;

    pthread_mutex_t q_mtx_{};

    std::string last_error_;
};
