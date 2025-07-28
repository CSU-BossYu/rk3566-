#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_MODE_RECOG  = 0,
    UI_MODE_ENROLL = 1,
    UI_MODE_MANAGE = 2,
} ui_mode_t;

typedef struct {
    int   x1;
    int   y1;
    int   x2;
    int   y2;
    float score;
} ui_face_box_t;

#ifdef __cplusplus
}
#endif
