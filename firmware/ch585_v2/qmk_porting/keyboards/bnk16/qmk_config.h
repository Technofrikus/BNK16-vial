/*
 * Binepad BNK16 board config.
 * Matrix, encoder and LED pins are measured on the board (see BNK16-Pinbelegung.txt);
 * layer switch, PB8, VBUS/charger/battery pins are NOT measured yet and are unused here.
 */
#pragma once

/* USB Device descriptor parameter (pid.codes test PID, private use) */
#define VENDOR_ID    0x1209
#define PRODUCT_ID   0x0001
#define DEVICE_VER   0x0001
#define MANUFACTURER BNK16Vial
#define PRODUCT      BNK16

/* Matrix (measured on the board): rows PB3 PB2 PB0 PB1 PB21 (row 4 = encoder clicks, found with the click probe; not PB20), cols PB5 PB4 PB6 PB7.
 * Col 4 = PB10 is the common line of the 4-position layer switch (traced on the PCB): its two other
 * lines go through diodes to rows 0 (PB3) and 1 (PB2), so the switch position reads as matrix (0,4)/(1,4).
 * PB10 is only free because USB runs on the HS port alone (usb_config.h). */
#define MATRIX_ROWS 5
#define MATRIX_COLS 5
#define MATRIX_ROW_PINS { B3, B2, B0, B1, B21 }
#define MATRIX_COL_PINS { B5, B4, B6, B7, B10 }
#define DIODE_DIRECTION  COL2ROW
#define HOLD_ON_OTHER_KEY_PRESS
#define EARLY_INIT_PERFORM_BOOTLOADER_JUMP FALSE

/* Encoders (measured): index 0 = small left, index 1 = small right, index 2 = big knob (no switch).
 * Small clicks are matrix (4,0) and (4,1). PB14/PB15 double as the TIO/TCK debug pins, PB16+ have
 * their own digital-input-disable bits; bnk16.c makes all of them plain GPIO inputs before encoder init. */
#define ENCODER_A_PINS { B14, B17, B8 }
#define ENCODER_B_PINS { B15, B16, B9 }
/* Resolution = pin edges per output step. Small knobs: 20 detents, one full quadrature cycle (4 edges)
 * per detent -> 4 (use 2 if a step only comes every second detent). Big knob: no detents, 20 cycles
 * per turn; 1 counts every intermediate state too (80 steps per turn). */
#define ENCODER_RESOLUTIONS { 4, 4, 1 }
/* Diagnostic: show raw encoder pin levels on the key LEDs (green = high, red = low), see bnk16.c */
// #define BNK16_ENCODER_DEBUG

/* RGB: WS2812 on PA10 (TMR1 PWM + DMA). Chain = 16 key LEDs (row-major) + 1 status LED. */
#define WS2812_DI_PIN A10
#define WS2812_DI_HIGH_DRIVE
/* LED supply is switched by PA11 (found with the pin scanner; PB8 from the stock code is big encoder B).
 * Level is an assumption: if the LEDs stay dark, change WS2812_EN_LEVEL to 0. */
#define WS2812_EN_PIN A11
#define WS2812_EN_LEVEL 1
#define WS2812_PWM_TARGET_PERIOD 800000 /* 800 kHz bit clock (driver default is 10x too slow) */
#define RGB_MATRIX_LED_COUNT 17
#define BNK16_STATUS_LED 16             /* chain index of the extra status LED, excluded from effects */
#define RGB_MATRIX_MAXIMUM_BRIGHTNESS 128
#define RGB_MATRIX_DEFAULT_VAL 96
#define RGB_MATRIX_DEFAULT_MODE RGB_MATRIX_CYCLE_LEFT_RIGHT
#define RGB_MATRIX_KEYPRESSES
#define ENABLE_RGB_MATRIX_SOLID_COLOR
#define ENABLE_RGB_MATRIX_BREATHING
#define ENABLE_RGB_MATRIX_BAND_SAT
#define ENABLE_RGB_MATRIX_CYCLE_ALL
#define ENABLE_RGB_MATRIX_CYCLE_LEFT_RIGHT
#define ENABLE_RGB_MATRIX_CYCLE_UP_DOWN
#define ENABLE_RGB_MATRIX_CYCLE_PINWHEEL
#define ENABLE_RGB_MATRIX_RAINBOW_MOVING_CHEVRON
/* Reactive effects (react to key presses, need RGB_MATRIX_KEYPRESSES above) */
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_SIMPLE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_WIDE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_MULTIWIDE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_CROSS
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_MULTICROSS
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_NEXUS
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_MULTINEXUS
#define ENABLE_RGB_MATRIX_SPLASH
#define ENABLE_RGB_MATRIX_MULTISPLASH
#define ENABLE_RGB_MATRIX_SOLID_SPLASH
#define ENABLE_RGB_MATRIX_SOLID_MULTISPLASH

#define DEBOUNCE 10

/* Vial */
#define VIAL_KEYBOARD_UID {0xDE, 0x8B, 0x27, 0x0F, 0x95, 0xED, 0x79, 0xBB}
/* Bring-up only: always unlocked. Remove once the keys work. */
#define VIAL_INSECURE
#define VIAL_UNLOCK_COMBO_ROWS {0, 0}
#define VIAL_UNLOCK_COMBO_COLS {0, 3}
#define DYNAMIC_KEYMAP_LAYER_COUNT 4

/* ========================================================================
 * Vial 高阶功能配置（界面可直接配置）
 * ======================================================================== */

/* 核心功能开关 — 确保 vial.h 能检测到并自动开启 VIAL_xxx_ENABLE */
#define TAP_DANCE_ENABLE
#define VIAL_TAP_DANCE_ENABLE
#define COMBO_ENABLE
#define VIAL_COMBO_ENABLE
#define KEY_OVERRIDE_ENABLE
#define VIAL_KEY_OVERRIDE_ENABLE
#define REPEAT_KEY_ENABLE
#define VIAL_ALT_REPEAT_KEY_ENABLE

/* Tap Dance 按键舞蹈：按1次/2次/3次/长按触发不同功能 */
#define VIAL_TAP_DANCE_COUNT 16
#define TAPPING_TERM 200
#define TAPPING_TERM_PER_KEY

/* Combo 组合键：同时按多个键触发一个功能 */
#define VIAL_COMBO_COUNT 16
#define COMBO_TERM 50

/* Key Override 按键覆盖：修饰键+普通键的组合重映射 */
#define VIAL_KEY_OVERRIDE_COUNT 16

/* ========================================================================
 * QMK 原生功能参数配置
 * ========================================================================
 * 注意：以下参数只有在 keymaps/default/rules.cmake 里把对应功能的
 * 开关改成 ON 之后才会生效。功能关闭时，这些参数定义了也不会被用到。
 * 想开启某个功能，去 rules.cmake 里找对应的 set(XXX_ENABLE OFF ...) 改成 ON
 * ======================================================================== */

/* --- One Shot 一次性修饰键 --- */
/* 按一下修饰键（Ctrl/Shift/Alt），它会保持到下一个普通按键，然后自动松开 */
#define ONESHOT_TAP_TOGGLE 5       /* 连按5次切换该修饰键的永久锁定状态 */
#define ONESHOT_TIMEOUT 5000        /* 一次性修饰键超时时间（毫秒），超时自动取消 */

/* --- Leader 引导键 --- */
/* 按一下 Leader 键，然后输入序列（如 L+B+T）触发一个功能，用少量按键扩展大量功能 */
#define LEADER_TIMEOUT 300           /* 引导序列超时时间（毫秒），超时取消序列 */
#define LEADER_PER_KEY_TIMING        /* 每个键单独计时，输入慢也不会超时 */

/* --- Auto Shift 自动换挡 --- */
/* 长按字母键自动输出大写，短按输出小写，可以省掉 Shift 键 */
#define AUTO_SHIFT_TIMEOUT 150       /* 长按多久算大写（毫秒） */
#define NO_AUTO_SHIFT_SPECIAL        /* 特殊键（F键/方向键等）不触发自动大写 */
#define NO_AUTO_SHIFT_NUMERIC        /* 数字键不触发自动大写 */

/* --- Space Cadet 太空学员 --- */
/* 左 Shift 单击输出 ( ，右 Shift 单击输出 ) ，长按还是 Shift */
/* 需要键盘上有左右两个 Shift 键才有意义 */

/* --- Caps Word 大写单词 --- */
/* 双击 Shift 后，下一个单词自动大写，输完空格/标点后自动恢复小写 */
#define BOTH_SHIFTS_TURNS_ON_CAPS_WORD  /* 同时按左右Shift触发大写单词模式 */
#define CAPS_WORD_IDLE_TIMEOUT 5000     /* 大写单词模式空闲超时（毫秒），超时自动退出 */

/* --- Secure 安全锁定 --- */
/* 锁定键盘，需要输入特定解锁序列才能使用，适合蓝牙键盘离开座位时锁键盘 */
#define SECURE_UNLOCK_TIMEOUT 5000   /* 解锁输入超时（毫秒），超时取消解锁 */
#define SECURE_IDLE_TIMEOUT 60000    /* 空闲多久自动锁定（毫秒），设0禁用自动锁 */

/* --- Achordion 和弦防误触 --- */
/* 修饰键需要另一只手的键先按时才生效，防止单手误触修饰键 */
/* 主要适合家用大键盘，小配列意义不大 */
#define ACHORDION_STREAK              /* 启用 Streak 模式（连续同手按键不触发） */

/* --- Sentence Case 句首大写 --- */
/* 检测到句号+空格后，下一个字母自动大写，打字体验优化 */

// #define BNK16_PIN_SCAN  /* diagnostic: LED supply pin scanner, see bnk16.c */
/* the USB port code overwrites the endpoint buffer, so press and release need a gap */
#define TAP_CODE_DELAY 30
/* diagnostic: types the state of all free pins over USB, to find the layer/mode/haptics switches (see bnk16.c) */
// #define BNK16_SWITCH_PROBE
