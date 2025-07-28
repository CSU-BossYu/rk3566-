#pragma once
#include <cstdint>

#include "utils/fixed_block_pool.h"
#include "ui/ui_types.h"

struct UiFramePacket {
    FixedBlockPool::Block blk{};  // blk.ptr: BGRA8888 tight

    int w = 0;
    int h = 0;

    ui_face_box_t boxes[4]{};     // 必须叫 boxes
    int boxes_n = 0;
};
