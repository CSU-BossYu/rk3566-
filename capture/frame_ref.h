#pragma once
#include <memory>
#include <cstdint>
#include "capture/v4l2_capture.h"

// 只保留 dma_fd + 元信息；最后一个引用释放时自动 requeue(index)
struct FrameMeta {
    int index = -1;
    int dma_fd = -1;
    int width = 0;
    int height = 0;
    int stride = 0;
    uint32_t pixfmt = 0;
    uint64_t ts_us = 0;
    V4L2Capture* cap = nullptr;
};

using FrameRef = std::shared_ptr<FrameMeta>;

static inline FrameRef make_frame_ref(V4L2Capture& cap, const V4L2Capture::RawFrame& rf) {
    // 注意：rf.index/rf.dma_fd 必须有效；rf 由 dequeue() 获得
    FrameMeta* p = new FrameMeta();
    p->index = rf.index;
    p->dma_fd = rf.dma_fd;
    p->width = rf.width;
    p->height = rf.height;
    p->stride = rf.stride;
    p->pixfmt = rf.pixfmt;
    p->ts_us = rf.ts_us;
    p->cap = &cap;

    // deleter：最后一个引用释放时归还 buffer
    return FrameRef(p, [](FrameMeta* m) {
        if (m) {
            if (m->cap && m->index >= 0) {
                m->cap->requeue(m->index);
            }
            delete m;
        }
    });
}
