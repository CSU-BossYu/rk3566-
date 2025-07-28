#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <vector>
#include <cstdio>

// 固定块内存池：预先分配 N 个 block，每个 block_size 字节。
// acquire() 取一个空闲块；release() 归还。
// 适用：RGB buffer、推理输入输出临时buffer等（固定大小，重复使用）。
class FixedBlockPool {
public:
    struct Block {
        void*    ptr = nullptr;   // 指向 block 内存
        uint32_t idx = 0;         // block 索引（release 用）
        uint32_t size = 0;        // block_size（便于调试/校验）
        explicit operator bool() const { return ptr != nullptr; }
    };

    FixedBlockPool() = default;

    FixedBlockPool(uint32_t block_size, uint32_t block_count, uint32_t alignment = 64) {
        init(block_size, block_count, alignment);
    }

    ~FixedBlockPool() {
        destroy();
    }

    FixedBlockPool(const FixedBlockPool&) = delete;
    FixedBlockPool& operator=(const FixedBlockPool&) = delete;

    bool init(uint32_t block_size, uint32_t block_count, uint32_t alignment = 64) {
        destroy();

        if (block_size == 0 || block_count == 0) return false;
        if (alignment < sizeof(void*)) alignment = sizeof(void*);

        block_size_  = block_size;
        block_count_ = block_count;
        alignment_   = alignment;

        // 为了对齐，实际每块按 alignment 向上取整
        stride_ = align_up(block_size_, alignment_);
        const size_t total = (size_t)stride_ * (size_t)block_count_;

        void* p = nullptr;
        int ret = posix_memalign(&p, alignment_, total);
        if (ret != 0 || !p) {
            std::fprintf(stderr, "[POOL][E] posix_memalign failed ret=%d\n", ret);
            reset_fields();
            return false;
        }
        base_ = (uint8_t*)p;
        // 预清零不是必须，但对减少“脏数据误判”有帮助
        memset(base_, 0, total);

        // free list 初始化：0..N-1
        free_.reserve(block_count_);
        for (uint32_t i = 0; i < block_count_; ++i) free_.push_back(i);

        pthread_mutex_init(&mtx_, nullptr);
        pthread_cond_init(&cv_, nullptr);
        inited_ = true;
        stop_ = false;
        return true;
    }

    void destroy() {
        if (!inited_) {
            reset_fields();
            return;
        }

        // 唤醒所有等待者，让其退出
        pthread_mutex_lock(&mtx_);
        stop_ = true;
        pthread_cond_broadcast(&cv_);
        pthread_mutex_unlock(&mtx_);

        pthread_cond_destroy(&cv_);
        pthread_mutex_destroy(&mtx_);

        if (base_) {
            free(base_);
            base_ = nullptr;
        }
        reset_fields();
    }

    bool isInited() const { return inited_; }
    uint32_t blockSize() const { return block_size_; }
    uint32_t blockCount() const { return block_count_; }
    uint32_t stride() const { return stride_; }

    // 阻塞获取：timeout_ms < 0 表示一直等；timeout_ms==0 表示不等直接返回
    bool acquire(Block& out, int timeout_ms = -1) {
        out = Block{};
        if (!inited_) return false;

        pthread_mutex_lock(&mtx_);
        while (!stop_ && free_.empty()) {
            if (timeout_ms == 0) {
                pthread_mutex_unlock(&mtx_);
                return false;
            }
            if (timeout_ms < 0) {
                pthread_cond_wait(&cv_, &mtx_);
            } else {
                // timedwait
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                add_ms(ts, timeout_ms);
                int ret = pthread_cond_timedwait(&cv_, &mtx_, &ts);
                if (ret == ETIMEDOUT) {
                    pthread_mutex_unlock(&mtx_);
                    return false;
                }
            }
        }

        if (stop_ || free_.empty()) {
            pthread_mutex_unlock(&mtx_);
            return false;
        }

        const uint32_t idx = free_.back();
        free_.pop_back();

        pthread_mutex_unlock(&mtx_);

        out.idx  = idx;
        out.size = block_size_;
        out.ptr  = (void*)(base_ + (size_t)idx * (size_t)stride_);
        return true;
    }

    // 归还 block
    void release(uint32_t idx) {
        if (!inited_) return;
        if (idx >= block_count_) return;

        pthread_mutex_lock(&mtx_);
        if (!stop_) {
            free_.push_back(idx);
            pthread_cond_signal(&cv_);
        }
        pthread_mutex_unlock(&mtx_);
    }

    // 可选：清空某个 block（调试用，线上通常不需要）
    void clear(uint32_t idx) {
        if (!inited_ || idx >= block_count_) return;
        void* p = (void*)(base_ + (size_t)idx * (size_t)stride_);
        memset(p, 0, block_size_);
    }

    // RAII：拿到后自动归还（推荐你后续线程改造时使用）
    class AutoBlock {
    public:
        AutoBlock() = default;
        AutoBlock(FixedBlockPool* pool, const Block& b) : pool_(pool), b_(b) {}
        ~AutoBlock() { reset(); }

        AutoBlock(const AutoBlock&) = delete;
        AutoBlock& operator=(const AutoBlock&) = delete;

        AutoBlock(AutoBlock&& o) noexcept { pool_ = o.pool_; b_ = o.b_; o.pool_ = nullptr; o.b_ = {}; }
        AutoBlock& operator=(AutoBlock&& o) noexcept {
            if (this != &o) {
                reset();
                pool_ = o.pool_;
                b_ = o.b_;
                o.pool_ = nullptr;
                o.b_ = {};
            }
            return *this;
        }

        const Block& get() const { return b_; }
        void* data() const { return b_.ptr; }
        uint32_t idx() const { return b_.idx; }
        uint32_t size() const { return b_.size; }
        explicit operator bool() const { return b_.ptr != nullptr; }

        void reset() {
            if (pool_ && b_.ptr) pool_->release(b_.idx);
            pool_ = nullptr;
            b_ = {};
        }

    private:
        FixedBlockPool* pool_ = nullptr;
        Block b_{};
    };

private:
    static inline uint32_t align_up(uint32_t x, uint32_t a) {
        return (x + a - 1) / a * a;
    }

    static inline void add_ms(timespec& ts, int ms) {
        const long ns_add = (long)(ms % 1000) * 1000000L;
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += ns_add;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
    }

    void reset_fields() {
        inited_ = false;
        stop_ = false;
        block_size_ = block_count_ = alignment_ = stride_ = 0;
        free_.clear();
    }

private:
    bool inited_ = false;
    bool stop_ = false;

    uint32_t block_size_  = 0;
    uint32_t block_count_ = 0;
    uint32_t alignment_   = 0;
    uint32_t stride_      = 0;

    uint8_t* base_ = nullptr;

    std::vector<uint32_t> free_;
    pthread_mutex_t mtx_{};
    pthread_cond_t  cv_{};
};
