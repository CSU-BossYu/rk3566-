#pragma once
#include <array>
#include <cstddef>
#include <pthread.h>
#include <utility>

// 一个轻量 ring-buffer 队列：
// - 支持 move-only
// - 固定容量（模板参数）
// - push/pop 可阻塞
// - close() 之后唤醒所有等待者，pop/push 返回 false
template<typename T, size_t Capacity = 8>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() {
        pthread_mutex_init(&mtx_, nullptr);
        pthread_cond_init(&cv_not_empty_, nullptr);
        pthread_cond_init(&cv_not_full_, nullptr);
    }

    ~ThreadSafeQueue() {
        close();
        pthread_cond_destroy(&cv_not_empty_);
        pthread_cond_destroy(&cv_not_full_);
        pthread_mutex_destroy(&mtx_);
    }

    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    void close() {
        pthread_mutex_lock(&mtx_);
        closed_ = true;
        pthread_cond_broadcast(&cv_not_empty_);
        pthread_cond_broadcast(&cv_not_full_);
        pthread_mutex_unlock(&mtx_);
    }

    bool is_closed() const {
        pthread_mutex_lock(&mtx_);
        bool c = closed_;
        pthread_mutex_unlock(&mtx_);
        return c;
    }

    // 阻塞 push（拷贝）
    bool push(const T& item) {
        pthread_mutex_lock(&mtx_);
        while (!closed_ && size_ == Capacity) {
            pthread_cond_wait(&cv_not_full_, &mtx_);
        }
        if (closed_) {
            pthread_mutex_unlock(&mtx_);
            return false;
        }
        buf_[tail_] = item;
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        pthread_cond_signal(&cv_not_empty_);
        pthread_mutex_unlock(&mtx_);
        return true;
    }

    // 阻塞 push（move）
    bool push(T&& item) {
        pthread_mutex_lock(&mtx_);
        while (!closed_ && size_ == Capacity) {
            pthread_cond_wait(&cv_not_full_, &mtx_);
        }
        if (closed_) {
            pthread_mutex_unlock(&mtx_);
            return false;
        }
        buf_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        pthread_cond_signal(&cv_not_empty_);
        pthread_mutex_unlock(&mtx_);
        return true;
    }

    // 非阻塞 try_push（move）
    bool try_push(T&& item) {
        pthread_mutex_lock(&mtx_);
        if (closed_ || size_ == Capacity) {
            pthread_mutex_unlock(&mtx_);
            return false;
        }
        buf_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        pthread_cond_signal(&cv_not_empty_);
        pthread_mutex_unlock(&mtx_);
        return true;
    }

    // 阻塞 pop：成功返回 true；close 且队列空返回 false
    bool pop(T& out) {
        pthread_mutex_lock(&mtx_);
        while (!closed_ && size_ == 0) {
            pthread_cond_wait(&cv_not_empty_, &mtx_);
        }
        if (size_ == 0 && closed_) {
            pthread_mutex_unlock(&mtx_);
            return false;
        }
        out = std::move(buf_[head_]);
        head_ = (head_ + 1) % Capacity;
        --size_;
        pthread_cond_signal(&cv_not_full_);
        pthread_mutex_unlock(&mtx_);
        return true;
    }

    // 非阻塞 try_pop
    bool try_pop(T& out) {
        pthread_mutex_lock(&mtx_);
        if (size_ == 0) {
            pthread_mutex_unlock(&mtx_);
            return false;
        }
        out = std::move(buf_[head_]);
        head_ = (head_ + 1) % Capacity;
        --size_;
        pthread_cond_signal(&cv_not_full_);
        pthread_mutex_unlock(&mtx_);
        return true;
    }

    size_t size() const {
        pthread_mutex_lock(&mtx_);
        size_t s = size_;
        pthread_mutex_unlock(&mtx_);
        return s;
    }

private:
    mutable pthread_mutex_t mtx_{};
    pthread_cond_t cv_not_empty_{};
    pthread_cond_t cv_not_full_{};

    std::array<T, Capacity> buf_{};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
    bool closed_ = false;
};
