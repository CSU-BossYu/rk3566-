#pragma once
#include <cstdint>

#include "ui_frame_packet.h"

template<typename T, size_t Capacity>
class ThreadSafeQueue;

class FixedBlockPool;
class UiRuntime {
public:
    UiRuntime() = default;
    ~UiRuntime() = default;

    void bind_frame_pipe(ThreadSafeQueue<UiFramePacket, 8>* q, FixedBlockPool* pool);
    void deinit(); // release current frame

    // UI thread only
    const UiFramePacket* current_frame() const { return has_cur_ ? &cur_ : nullptr; }

    // UI thread only: drain latest; manage_mode=true -> release all and clear current
    void drain_latest(bool manage_mode);

private:
    ThreadSafeQueue<UiFramePacket, 8>* q_{nullptr};
    FixedBlockPool* pool_{nullptr};

    UiFramePacket cur_{};
    bool has_cur_{false};
};
