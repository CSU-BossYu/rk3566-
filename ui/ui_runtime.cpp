#include "ui_runtime.h"
#include "utils/thread_safe_queue.h"
#include "utils/fixed_block_pool.h"

void UiRuntime::bind_frame_pipe(ThreadSafeQueue<UiFramePacket, 8>* q, FixedBlockPool* pool) {
    q_ = q;
    pool_ = pool;
}

void UiRuntime::deinit() {
    if (has_cur_ && pool_ && cur_.blk.ptr) {
        pool_->release(cur_.blk.idx);
    }
    has_cur_ = false;
    cur_ = UiFramePacket{};
    q_ = nullptr;
    pool_ = nullptr;
}

void UiRuntime::drain_latest(bool manage_mode) {
    if (!q_ || !pool_) return;

    UiFramePacket pkt{};
    bool got = false;
    UiFramePacket last{};

    while (q_->try_pop(pkt)) {
        if (got && last.blk.ptr) pool_->release(last.blk.idx);
        last = pkt;
        got = true;
    }

    if (manage_mode) {
        if (got && last.blk.ptr) pool_->release(last.blk.idx);
        if (has_cur_ && cur_.blk.ptr) pool_->release(cur_.blk.idx);
        has_cur_ = false;
        cur_ = UiFramePacket{};
        return;
    }

    if (!got) return;

    if (has_cur_ && cur_.blk.ptr) pool_->release(cur_.blk.idx);
    cur_ = last;
    has_cur_ = true;
}
