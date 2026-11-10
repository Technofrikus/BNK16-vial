/*
 * Non-blocking haptic patterns for test B, run as a TMOS task. The first build buzzed with busy waits
 * inside the GAP callbacks (190 ms right at link setup), which blocked the stack and the link never came
 * up. Here every half period is a TMOS timer, so the stack keeps running (and the chip may sleep in the
 * pauses). Same drive as bnk16_test.h: PA7 low = motor on, push on every second half period.
 */
#include "CONFIG.h"
#include "haptic.h"

#define HAPTIC_EVT  0x0001
#define HALF_PERIOD 6 /* TMOS ticks of 625 us: 3.75 ms, about 133 Hz */
#define QUEUE_LEN   16

typedef struct {
    uint8_t  pushes;
    uint16_t pause_ms;
} haptic_step_t;

static uint8_t       haptic_task = INVALID_TASK_ID;
static haptic_step_t queue[QUEUE_LEN];
static uint8_t       q_head, q_len;
static uint8_t       slot, slots;
static uint16_t      pause_ms;
static uint8_t       pause_pending;
static uint8_t       running;

static uint16_t haptic_event(uint8_t task_id, uint16_t events) {
    if (events & SYS_EVENT_MSG) {
        uint8_t *msg;
        while ((msg = tmos_msg_receive(task_id)) != NULL) {
            tmos_msg_deallocate(msg);
        }
        return events ^ SYS_EVENT_MSG;
    }
    if (events & HAPTIC_EVT) {
        if (slot < slots) {
            if (slot & 1) {
                GPIOA_SetBits(GPIO_Pin_7);
            } else {
                GPIOA_ResetBits(GPIO_Pin_7);
            }
            slot++;
            tmos_start_task(haptic_task, HAPTIC_EVT, HALF_PERIOD);
        } else {
            GPIOA_SetBits(GPIO_Pin_7);
            if (pause_pending && pause_ms) {
                pause_pending = 0;
                tmos_start_task(haptic_task, HAPTIC_EVT, MS1_TO_SYSTEM_TIME(pause_ms));
            } else if (q_len) {
                haptic_step_t s = queue[q_head];
                q_head          = (q_head + 1) % QUEUE_LEN;
                q_len--;
                slots         = s.pushes ? 2 * s.pushes - 1 : 0;
                slot          = 0;
                pause_ms      = s.pause_ms;
                pause_pending = 1;
                tmos_set_event(haptic_task, HAPTIC_EVT);
            } else {
                running = 0;
            }
        }
        return events ^ HAPTIC_EVT;
    }
    return 0;
}

void haptic_init(void) {
    haptic_task = TMOS_ProcessEventRegister(haptic_event);
}

void haptic_queue(uint8_t pushes, uint16_t pause_after_ms) {
    if (q_len >= QUEUE_LEN) {
        return;
    }
    queue[(q_head + q_len) % QUEUE_LEN] = (haptic_step_t){ pushes, pause_after_ms };
    q_len++;
    if (!running) {
        running = 1;
        slot = slots = 0;
        pause_pending = 0;
        tmos_set_event(haptic_task, HAPTIC_EVT);
    }
}
