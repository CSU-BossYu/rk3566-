#pragma once
#include <cstddef>
#include <cstdint>

#include "ui_types.h"
#include "ui_frame_packet.h"
#include "utils/fixed_block_pool.h"

template<typename T, size_t Capacity>
class ThreadSafeQueue;

#ifdef __cplusplus
extern "C" {
#endif

void ui_app_init(void);
void ui_app_deinit(void);
void ui_app_tick(void);

ui_mode_t ui_app_get_mode(void);
void      ui_app_set_mode(ui_mode_t m);

int       ui_app_get_selected(void);

/* actions: UI -> Vision */
uint32_t  ui_app_poll_actions(void);

#define UI_ACT_START_ENROLL   (1u<<0)
#define UI_ACT_DELETE         (1u<<1)
#define UI_ACT_CANCEL_ENROLL  (1u<<2)

/* status/info from Vision -> UI */
void ui_app_set_status(const char* s);
void ui_app_set_manage_list(const char** names, int n);

/* UI frame pipe: RgaWorker -> UI */
void ui_app_bind_frame_pipe(ThreadSafeQueue<UiFramePacket, 8>* q,
                            FixedBlockPool* pool);

/* Vision -> UI overlay boxes (UI 480x480 coordinate) */
void ui_app_set_overlay_boxes(const ui_face_box_t* boxes, int n);

/* Enroll name getter (Vision reads it after START_ENROLL) */
int ui_app_get_enroll_name(char* out, int cap);

/* Vision -> UI: after successful enroll, reset ENROLL state machine to IDLE */
void ui_app_notify_enroll_done(void);

/* Vision -> UI: clear Manage selection after delete */
void ui_app_clear_selected(void);

#ifdef __cplusplus
}
#endif