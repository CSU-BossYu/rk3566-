#pragma once
#include <cstdint>

struct FaceBox {
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float score = 0;

    // 5-point landmarks in detector input space (e.g. 320x320)
    // order: left_eye, right_eye, nose, left_mouth, right_mouth
    float kps[10] = {0};
};
