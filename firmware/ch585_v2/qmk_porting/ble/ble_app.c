/*
 * ble_app.c — BLE 蓝牙应用层核心实现
 *
 * 基于沁恒官方 HID_Keyboard 示例移植，适配 QMK 主循环。
 * 负责：协议栈初始化、GAP 事件处理、HID 报告发送、LED 状态联动。
 */

#include "ble_app.h"
#include "ble_config.h"
#include "led_indicator.h"
#include "config.h"
#include "HAL.h"
#include "hiddev.h"
#include "hidkbdservice.h"
#include "battservice.h"
#include "devinfoservice.h"
#include "hidkbd.h"
#include "CH58xBLE_LIB.h"
#include "timer.h"   /* timer_read32 — 超时判断 */

/* 有线模式激活标志 — 定义在 ch585_evb.c
 * 有线模式下，蓝牙状态变化不应覆盖 LED 状态（LED 常亮表示有线）
 */
extern bool wired_mode_active;

/* ========================================================================
 * 内部状态变量
 * ======================================================================== */

static uint8_t ble_task_id = INVALID_TASK_ID;
static uint16_t conn_handle = GAP_CONNHANDLE_INIT;
static ble_state_t current_state = BLE_STATE_IDLE;

/* ========================================================================
 * 三设备切换 — 绑定表（3个槽位，EEPROM持久化）
 * ======================================================================== */

#define BLE_SEND_WAIT_MS        500     /* longest wait for a free packet buffer */
#define BLE_MAX_DEVICES         3       /* 最多3个设备槽位 */
#define BLE_BOND_TABLE_MAGIC    0xA5    /* 魔数，判断 EEPROM 数据是否有效 */
/* The table has its own data flash page outside the QMK area, accessed with EEPROM_READ/ERASE/WRITE.
 * QMK offset 0x400 (the old place) is inside Vial's dynamic macro buffer. */
#define BLE_BOND_TABLE_FLASH_ADDR   USER_EEPROM_START_POSITION
#define BLE_BOND_SAVE_DELAY_MS      2000    /* flash writes wait until pairing has settled */
#define BLE_SWITCH_RETRY_MS         500     /* re-check a slot change that is still waiting for the stack */
#define BLE_SWITCH_MAX_RETRIES      5
#define BLE_RECONNECT_TIMEOUT   30000   /* 回连总超时：30秒（定向广播1.28秒+普通广播约28秒） */
#define BLE_PAIRING_TIMEOUT     180000  /* 配对超时：3分钟（量产键盘标准） */
#define BLE_DIRECTED_ADV_TIMEOUT 1500   /* 定向广播协议栈超时1.28秒，留余量1.5秒后转普通广播 */
#define BLE_ENCRYPT_TIMEOUT_MS  10000   /* a link that may not pair must be encrypted by then, else it is dropped */
/* The stack writes the last part of a bond (the host's notification settings) only when the link ends.
 * After a power cut that record is incomplete, and the stack formats the whole SNV area at the next
 * boot (seen in phase 44 dumps). So the bond is saved this long after every encryption. */
#define BLE_BOND_UPDATE_DELAY_MS 5000
/* Storage layout. 1: addresses from the chip MAC, SNV area erased once (stock bonds at 0x7800). An older
 * table makes ble_app_init() erase the stack's bonds and empty the slots, so every host pairs again. */
#define BLE_BOND_TABLE_LAYOUT   1

/* 单个设备的绑定信息 */
typedef struct {
    uint8_t addr_type;              /* 地址类型：0=公共地址，1=随机地址 */
    uint8_t bd_addr[6];             /* 设备 BD_ADDR（6字节） */
    uint8_t paired;                  /* 是否已配对：0=未配对，1=已配对 */
} ble_device_info_t;

/* Complete bond table as stored in data flash. The mode is not stored: the slide switch decides it. */
typedef struct {
    uint8_t magic;                   /* 魔数 BLE_BOND_TABLE_MAGIC */
    uint8_t current_device;          /* 当前选中的设备槽位（1-3） */
    uint8_t snv_erase_pending;       /* the stack's bonds still have to be erased (survives a reset) */
    uint8_t layout;                  /* BLE_BOND_TABLE_LAYOUT; 0 in tables from before phase 44 */
    ble_device_info_t devices[BLE_MAX_DEVICES];  /* 3个设备槽位 */
} ble_bond_table_t;

_Static_assert(sizeof(ble_bond_table_t) <= EEPROM_PAGE_SIZE, "BLE bond table does not fit its page");

static ble_bond_table_t bond_table __attribute__((aligned(4)));  /* 内存中的设备绑定表 */
static bool bond_table_dirty = false;      /* changed, not yet written to flash */
static uint32_t bond_table_dirty_time = 0;
static uint32_t state_enter_time = 0;  /* 进入当前状态的时间（用于超时判断） */

/* 待切换/待配对目标（断开完成后执行）*/
static uint8_t pending_switch_device = 0;
static uint8_t pending_pair_device = 0;
static uint8_t switch_retries = 0;

static bool stack_started = false;  /* GAPROLE_STARTED seen: GAP_ConfigDeviceAddr returns bleNotReady before it */
static bool adv_active = false;     /* advertising requested or running; cleared by GAPROLE_WAITING */

/* Peer of the current link, goes into the bond table once pairing succeeded */
static uint8_t conn_peer_addr_type = 0;
static uint8_t conn_peer_addr[B_ADDR_LEN];
static bool conn_peer_valid = false;

/* Pairing gate: a new host may only pair in a pairing hold or on an empty slot. Any other link must be
 * encrypted with an existing bond within BLE_ENCRYPT_TIMEOUT_MS. */
static bool link_may_pair = false;
static bool link_secure = false;
static uint32_t link_start_time = 0;
static bool bond_update_pending = false;
static uint32_t link_secure_time = 0;

/* HID 报告去重：上一次成功发送的报告
 * 大连接间隔(500/1000ms)下，矩阵扫描重复生成相同报告，
 * HidDev_Report 内部缓存排队，导致连击（按1次输出10多个字符）。
 */
/* 待重试的报告（发送失败时保存，主循环里重试）*/
static uint8_t pending_report[HID_KEYBOARD_IN_RPT_LEN] = {0};
static bool pending_report_valid = false;

/* Mouse, system and consumer reports are retried the same way, one slot per report ID */
#define BLE_EXTRA_RPT_MAX_LEN   5
typedef struct {
    uint8_t id;
    uint8_t len;
    bool valid;
    uint8_t data[BLE_EXTRA_RPT_MAX_LEN];
} ble_pending_extra_t;

static ble_pending_extra_t pending_extra[] = {
    {.id = HID_RPT_ID_MOUSE_IN},
    {.id = HID_RPT_ID_SYSTEM_IN},
    {.id = HID_RPT_ID_CONSUMER_IN},
};
#define BLE_PENDING_EXTRA_COUNT (sizeof(pending_extra) / sizeof(pending_extra[0]))

/* Connection interval state, see the end of this file */
static uint8_t current_interval_level = 0;
static uint32_t last_param_update_time = 0;
static uint16_t fixed_conn_interval = 0;   /* mobile mode, 0 = none */

/* 前向声明 */
static void bond_table_load(void);
static void bond_table_save(void);
static bool bond_table_is_paired(uint8_t device_idx);
static void bond_table_set_device(uint8_t device_idx, uint8_t addr_type, uint8_t *bd_addr);
static void bond_table_clear_all(void);
static void ble_start_directed_advertising(uint8_t addr_type, uint8_t *bd_addr);
static void ble_start_general_advertising(void);
static void ble_check_timeout(void);
static void ble_set_device_name(uint8_t device_idx);
static void ble_request_slot(uint8_t device_idx, bool pairing);
static void ble_run_pending(void);
static void ble_erase_stack_bonds(void);
static void ble_set_pairing_allowed(bool allowed);
static void ble_start_reconnect(void);

/* ========================================================================
 * 广播数据
 *
 * 广播包内容：标志位 + 外观（HID键盘） + HID 服务 UUID
 * 扫描响应包：设备名 + 连接间隔范围 + 电池服务 UUID + 发射功率
 * ======================================================================== */

/* 广播数据 — 包含标志位和外观（和官方 HID_Keyboard 示例完全一致） */
static uint8_t advertData[] = {
    /* 标志位 */
    0x02,                           /* 长度 */
    GAP_ADTYPE_FLAGS,               /* 类型：标志位 */
    GAP_ADTYPE_FLAGS_LIMITED | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,  /* 有限可发现，不支持 BR/EDR（和官方一致） */

    /* 外观 */
    0x03,                           /* 长度 */
    GAP_ADTYPE_APPEARANCE,          /* 类型：外观 */
    LO_UINT16(GAP_APPEARE_HID_KEYBOARD),  /* HID 键盘外观低字节 */
    HI_UINT16(GAP_APPEARE_HID_KEYBOARD),  /* HID 键盘外观高字节 */
};

/* One name per slot, as on the stock firmware. Scan response: 2 + 9 (name) + 6 + 6 = 23 of 31 bytes. */
static void ble_set_device_name(uint8_t device_idx)
{
    static const char *device_names[BLE_MAX_DEVICES] = {
        "BNK16 BT1",
        "BNK16 BT2",
        "BNK16 BT3",
    };
    if (device_idx < 1 || device_idx > BLE_MAX_DEVICES) {
        return;
    }

    const char *name = device_names[device_idx - 1];
    uint8_t name_len = (uint8_t)strlen(name);

    /* 构建扫描响应数据 */
    uint8_t scan_rsp[31];
    uint8_t offset = 0;

    /* 完整本地名称 */
    scan_rsp[offset++] = name_len + 1;
    scan_rsp[offset++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
    memcpy(&scan_rsp[offset], name, name_len);
    offset += name_len;

    /* 从机连接间隔范围 */
    scan_rsp[offset++] = 0x05;
    scan_rsp[offset++] = GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE;
    scan_rsp[offset++] = LO_UINT16(BLE_MIN_CONN_INTERVAL);
    scan_rsp[offset++] = HI_UINT16(BLE_MIN_CONN_INTERVAL);
    scan_rsp[offset++] = LO_UINT16(BLE_MAX_CONN_INTERVAL);
    scan_rsp[offset++] = HI_UINT16(BLE_MAX_CONN_INTERVAL);

    /* 服务 UUID（HID + 电池）*/
    scan_rsp[offset++] = 0x05;
    scan_rsp[offset++] = GAP_ADTYPE_16BIT_MORE;
    scan_rsp[offset++] = LO_UINT16(HID_SERV_UUID);
    scan_rsp[offset++] = HI_UINT16(HID_SERV_UUID);
    scan_rsp[offset++] = LO_UINT16(BATT_SERV_UUID);
    scan_rsp[offset++] = HI_UINT16(BATT_SERV_UUID);

    /* 注意：发射功率字段已去掉（可选字段，不影响功能，腾出3字节给设备名）*/

    /* 更新 GAPRole 扫描响应数据（广播时生效）*/
    GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, offset, scan_rsp);

    /* 更新 GGS 服务设备名（连接后主机读取到的名字）*/
    {
        uint8_t gap_name[GAP_DEVICE_NAME_LEN] = {0};
        memcpy(gap_name, name, name_len);
        GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, gap_name);
    }
}

/* ========================================================================
 * 不同槽位不同蓝牙地址
 *
 * 为什么需要不同地址：
 *   从设备1切换到设备2时，设备2启动普通可连接广播。
 *   但已配对过设备1的电脑（Windows/Mac）记录的是蓝牙地址，看到同地址在广播
 *   就会主动重新连接，导致"切换到设备2但自动回连到电脑"，手机搜不到。
 *
 * 解决方案：每个槽位用不同的静态随机地址（Static Random Address）。
 *   电脑配对过设备1的地址A，看到设备2的地址B（没配对过），不会主动连接，
 *   手机就能正常搜到并配对。这也是量产键盘的标准做法。
 *
 * 静态随机地址要求：
 *   - The top two bits of the most significant byte must be 11. The stack keeps addresses LSB first
 *     (bleConfig_t.MacAddr "little-endian"; SM_CalcRandomAddr sets the address type bits in byte 5),
 *     so that is the last byte below. The first byte also has them set, so the values are valid
 *     in either byte order.
 *   - 不能全 0 或全 1
 *   - 每个槽位用一个固定地址，配对信息和地址一一对应
 *
 * The address is set with no link and no advertising active, before advertising starts, and not before
 * GAPROLE_STARTED: see ble_request_slot() and ble_run_pending().
 * ======================================================================== */

/* Static random address of each slot, LSB first. Built from the chip's MAC, so every board has its
 * own: the first byte differs per slot, the top two bits of the last byte are set (static random).
 * The fixed table is only the fallback for a MAC that would give an invalid address. */
static const uint8_t device_addresses[BLE_MAX_DEVICES][B_ADDR_LEN] = {
    {0xC1, 0x16, 0x85, 0x35, 0x4E, 0xC1},  /* 设备1 */
    {0xC2, 0x16, 0x85, 0x35, 0x4E, 0xC2},  /* 设备2 */
    {0xC3, 0x16, 0x85, 0x35, 0x4E, 0xC3},  /* 设备3 */
};

static void ble_get_device_address(uint8_t device_idx, uint8_t *addr)
{
    uint8_t mac[B_ADDR_LEN];
    bool all_ones = true;
    bool random_zero = true;

    GetMACAddress(mac);
    memcpy(addr, mac, B_ADDR_LEN);
    addr[0] ^= (uint8_t)(device_idx - 1);
    addr[B_ADDR_LEN - 1] |= 0xC0;

    /* the 46 random bits must not be all 0 or all 1 */
    for (uint8_t i = 0; i < B_ADDR_LEN; i++) {
        uint8_t random_bits = i == B_ADDR_LEN - 1 ? (addr[i] & 0x3F) : addr[i];
        uint8_t ones = i == B_ADDR_LEN - 1 ? 0x3F : 0xFF;
        all_ones = all_ones && random_bits == ones;
        random_zero = random_zero && random_bits == 0;
    }
    if (all_ones || random_zero) {
        memcpy(addr, device_addresses[device_idx - 1], B_ADDR_LEN);
    }
}

/* Returns false if the stack refused the address; the caller retries */
static bool ble_set_device_address(uint8_t device_idx)
{
    uint8_t addr[B_ADDR_LEN];

    if (device_idx < 1 || device_idx > BLE_MAX_DEVICES) {
        return false;
    }

    ble_get_device_address(device_idx, addr);
    return GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, addr) == SUCCESS;
}

/* ========================================================================
 * HID 设备配置
 * ======================================================================== */

static hidDevCfg_t ble_hid_cfg = {
    HID_IDLE_TIMEOUT,               /* Idle 超时（毫秒），0=禁用 */
    HID_FEATURE_FLAGS,              /* HID 特性标志 */
};

/* ========================================================================
 * 内部函数声明
 * ======================================================================== */

static uint16_t ble_process_event(uint8_t task_id, uint16_t events);
static void ble_process_tmos_msg(tmos_event_hdr_t *pMsg);
static uint8_t ble_hid_report_cb(uint8_t id, uint8_t type, uint16_t uuid,
                                  uint8_t oper, uint16_t *pLen, uint8_t *pData);
static void ble_hid_event_cb(uint8_t evt);
static void ble_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent);

/* ========================================================================
 * HID 设备回调函数表
 *
 * hidDevCB_t 结构包含四个回调：
 * - pfnRptCB: 报告读写回调（主机读/写 HID 报告时调用）
 * - pfnEvtCB: HID 事件回调（挂起/退出挂起/设置启动模式/设置报告模式）
 * - pfnPasscodeCB: 配对密码回调（需要 MITM 时用）
 * - pfnStateCB: GAP 状态变化回调（连接/断开/广播等）
 * ======================================================================== */

static hidDevCB_t ble_hid_cbs = {
    ble_hid_report_cb,    /* 报告回调 */
    ble_hid_event_cb,     /* 事件回调 */
    NULL,                  /* 密码回调（不需要 MITM，设为 NULL） */
    ble_state_cb,          /* 状态变化回调 */
};

/* ========================================================================
 * BLE 中断处理函数
 *
 * 注意：LLE_IRQHandler 已经在 BLE 协议栈库的 ble_task_scheduler.S 里定义了
 * （强符号，放在 .highcode 段，手动保存/恢复所有31个寄存器，读取
 * g_LLE_IRQLibHandlerLocation 函数指针间接调用，最后 mret 返回）。
 * 这里不能用 C 语言重复定义 LLE_IRQHandler，否则编译器只保存部分寄存器，
 * 导致 BLE 中断处理不正确，射频无法正常工作。
 *
 * BB_IRQHandler 启动文件里的弱定义是死循环，这里强定义覆盖，指向 BLE 协议栈库。
 * ======================================================================== */

void BB_IRQHandler(void)
{
    BB_IRQLibHandler();  /* BLE 基带中断 — 协议栈库处理 */
}

/* ========================================================================
 * 初始化
 * ======================================================================== */

void ble_app_init(void)
{
    /* BLE 协议栈初始化完成提示 — LED0 常亮2s后熄灭 */
    led_set_conn_state(LED_CONN_SUCCESS);

    /* Load the bond table (pairing info of the 3 slots and the selected slot) from data flash */
    bond_table_load();

    /* 注意：CH58X_BLEInit() 和 HAL_Init() 已经在 platform_setup() 里调用过了，
     * 这里不能重复调用，否则 BLE 协议栈会重复初始化，导致内存池混乱、广播无法发出。
     * 官方示例里这两个函数只在 main 函数里调用一次。
     */

    /* 1. 初始化 GAP 外设角色
     * 必须调用，否则无法作为从设备广播和连接
     */
    GAPRole_PeripheralInit();

    /* 2. 初始化 HID 设备服务
     * 必须调用，否则 HID 服务无法正常工作
     */
    HidDev_Init();

    /* 5. 注册 TMOS 任务事件处理函数 */
    ble_task_id = TMOS_ProcessEventRegister(ble_process_event);

    /* 4. 设置 GAP 外设角色参数 */
    {
        /* No advertising at init: the GAPROLE_STARTED case starts the current slot if the board is
         * in BT mode. */
        uint8_t initial_advertising_enable = FALSE;
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);
        /* Scan response and GGS name are set per slot by ble_set_device_name() */
        ble_set_device_name(bond_table.current_device);
    }

    /* 5. GGS 设备名已经在 ble_set_device_name() 里设置了，这里不重复设置 */

    /* 6. 设置 GAP Bond Manager（配对管理器）参数 */
    {
        uint32_t passkey = BLE_DEFAULT_PASSCODE;
        uint8_t pairMode = BLE_PAIRING_MODE;
        uint8_t mitm = BLE_MITM_MODE;
        uint8_t ioCap = BLE_IO_CAPABILITIES;
        uint8_t bonding = BLE_BONDING_MODE;
        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    /* 7. 设置电池服务临界电量 */
    {
        uint8_t critical = BATT_CRITICAL_LEVEL;
        Batt_SetParameter(BATT_PARAM_CRITICAL_LEVEL, sizeof(uint8_t), &critical);
    }

    /* 8. 添加 HID 键盘服务 */
    Hid_AddService();

    /* 9. 注册 HID 设备配置和回调 */
    HidDev_Register(&ble_hid_cfg, &ble_hid_cbs);

    /* 10. 设置启动事件（和官方 HID_Keyboard 示例完全一致）
     * 虽然我们的 START_DEVICE_EVT 处理函数里只清除事件（广播启动由 hiddev.c 内部处理），
     * 但官方示例也设置了这个事件，为完全对齐加上。
     */
    tmos_set_event(ble_task_id, START_DEVICE_EVT);

    current_state = BLE_STATE_IDLE;
    conn_handle = GAP_CONNHANDLE_INIT;
}

/* ========================================================================
 * TMOS 任务事件处理
 * ======================================================================== */

static uint16_t ble_process_event(uint8_t task_id, uint16_t events)
{
    /* 处理系统消息事件 */
    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg;
        if ((pMsg = tmos_msg_receive(ble_task_id)) != NULL) {
            ble_process_tmos_msg((tmos_event_hdr_t *)pMsg);
            tmos_msg_deallocate(pMsg);
        }
        return (events ^ SYS_EVENT_MSG);
    }

    /* 启动设备事件 — 启动广播由 hiddev.c 内部处理，这里只清除事件 */
    if (events & START_DEVICE_EVT) {
        return (events ^ START_DEVICE_EVT);
    }

    /* 参数更新事件 — 请求更新连接间隔 */
    if (events & START_PARAM_UPDATE_EVT) {
        if (current_state == BLE_STATE_CONNECTED && fixed_conn_interval != 0) {
            ble_set_conn_interval_raw(fixed_conn_interval);
        } else if (current_state == BLE_STATE_CONNECTED) {
            GAPRole_PeripheralConnParamUpdateReq(conn_handle,
                BLE_MIN_CONN_INTERVAL,
                BLE_MAX_CONN_INTERVAL,
                BLE_SLAVE_LATENCY,
                BLE_CONN_TIMEOUT,
                ble_task_id);
            /* this asks for the fast level, so power_task() can move on from there if the board is asleep */
            current_interval_level = 0;
            last_param_update_time = timer_read32();
        }
        return (events ^ START_PARAM_UPDATE_EVT);
    }

    return 0;  /* 未处理的事件 */
}

/* ========================================================================
 * TMOS 消息处理
 * ======================================================================== */

static void ble_process_tmos_msg(tmos_event_hdr_t *pMsg)
{
    /* 目前没有需要处理的自定义消息 */
    (void)pMsg;
}

/* ========================================================================
 * GAP 状态变化回调
 *
 * 当 BLE 连接状态变化时调用（广播开始、连接建立、连接断开等）
 * 参数：
 *   newState - 新状态（需要和 GAPROLE_STATE_ADV_MASK 掩码后判断）
 *   pEvent - 事件详情（包含连接句柄、断开原因等）
 * ======================================================================== */

static void ble_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    switch (newState & GAPROLE_STATE_ADV_MASK) {
        case GAPROLE_STARTED:
            /* Stack is up: addresses can be set from here on. Runs the current slot, or a slot
             * requested before this point. In wired mode only the address is set. */
            stack_started = true;
            ble_erase_stack_bonds();
            if (wired_mode_active) {
                ble_set_device_address(bond_table.current_device);
            } else {
                if (pending_switch_device == 0 && pending_pair_device == 0) {
                    pending_switch_device = bond_table.current_device;
                }
                ble_run_pending();
            }
            break;

        case GAPROLE_ADVERTISING:
            /* 正在广播 */
            if (pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT) {
                adv_active = true;
                if (wired_mode_active ||
                    (current_state != BLE_STATE_ADVERTISING &&
                     current_state != BLE_STATE_RECONNECTING &&
                     current_state != BLE_STATE_PAIRING)) {
                    /* Not wanted (wired mode, or stopped while it was starting). The stack only ends
                     * advertising on an enabled -> disabled change of this flag, and the flag may
                     * already be off. */
                    uint8_t enable = TRUE;
                    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &enable);
                    enable = FALSE;
                    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &enable);
                }
            }
            break;

        case GAPROLE_CONNECTED:
            /* 连接建立成功 */
            if (pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT) {
                gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;
                conn_handle = event->connectionHandle;
                adv_active = false;

                /* A host connected just as the board left BT mode or changed slot: drop it */
                if (wired_mode_active || pending_switch_device != 0 || pending_pair_device != 0) {
                    current_state = BLE_STATE_SWITCHING;
                    state_enter_time = timer_read32();
                    ble_disconnect();
                    break;
                }

                /* Pairing is open only if it was open while advertising (ble_run_pending()) */
                link_may_pair = current_state == BLE_STATE_PAIRING || current_state == BLE_STATE_ADVERTISING ||
                                !bond_table_is_paired(bond_table.current_device);
                link_secure = false;
                link_start_time = timer_read32();

                current_state = BLE_STATE_CONNECTED;

                /* 重置待重试队列：重新连接后清空旧报告 */
                pending_report_valid = false;
                for (uint8_t i = 0; i < BLE_PENDING_EXTRA_COUNT; i++) {
                    pending_extra[i].valid = false;
                }

                if (!wired_mode_active) {
                    led_set_conn_state(LED_CONN_SUCCESS);  /* 常亮2秒表示连接成功，然后自动熄灭 */
                }

                /* Remember the peer; it goes into the bond table in ble_app_pair_state() once pairing
                 * succeeded (the slot is not marked paired before that). */
                conn_peer_addr_type = event->devAddrType;
                memcpy(conn_peer_addr, event->devAddr, B_ADDR_LEN);
                conn_peer_valid = true;

                /* 延迟请求更新连接参数（让主机先完成服务发现） */
                tmos_start_task(ble_task_id, START_PARAM_UPDATE_EVT, 12800);
            }
            break;

        case GAPROLE_CONNECTED_ADV:
            /* 连接中同时广播（多连接时） */
            break;

        case GAPROLE_WAITING:
            /* Link ended or advertising ended. The stack cleared its advertising flag before this
             * callback, so advertising can be started from here.
             *
             * Highest priority first:
             * 1. wired mode: stay off
             * 2. a slot change is pending (pending_switch_device, pending_pair_device): run it
             * 3. RECONNECTING, directed advertising timed out: general advertising for the rest of the
             *    30 s. The library reports this as opcode GAP_LINK_ESTABLISHED_EVENT with status
             *    bleGAPConnNotAcceptable; state_enter_time is not touched, the 30 s are the total.
             * 4. RECONNECTING, a link ended: directed advertising again, same 30 s
             * 5. ADVERTISING, PAIRING: advertising ended: restart it
             * 6. CONNECTED: the link dropped: reconnect to the current slot
             * SWITCHING with nothing pending, IDLE, STANDBY: nothing to do
             */
            conn_handle = GAP_CONNHANDLE_INIT;
            conn_peer_valid = false;
            link_secure = false;
            bond_update_pending = false;
            adv_active = false;

            ble_erase_stack_bonds();

            if (wired_mode_active) {
                /* hiddev.c can re-enable advertising on a failed pairing just before this callback */
                ble_stop_advertising();
                current_state = BLE_STATE_IDLE;
                break;
            }

            if (pending_switch_device != 0 || pending_pair_device != 0) {
                ble_run_pending();
                break;
            }

            if (current_state == BLE_STATE_RECONNECTING) {
                if (pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT) {
                    ble_start_reconnect();
                } else {
                    ble_start_general_advertising();
                    led_set_conn_state(LED_CONN_RECONNECTING);
                }
            } else if (current_state == BLE_STATE_ADVERTISING || current_state == BLE_STATE_PAIRING) {
                ble_start_general_advertising();
                led_set_conn_state(LED_CONN_PAIRING);
            } else if (current_state == BLE_STATE_CONNECTED) {
                pending_switch_device = bond_table.current_device;
                ble_run_pending();
            }
            break;

        case GAPROLE_ERROR:
            /* 错误 */
            adv_active = false;
            current_state = BLE_STATE_IDLE;
            if (!wired_mode_active) {
                led_set_conn_state(LED_CONN_IDLE);
            }
            break;

        default:
            break;
    }
}

/* ========================================================================
 * HID 事件回调
 *
 * 处理 HID 服务相关事件（挂起、退出挂起、设置启动模式、设置报告模式）
 * ======================================================================== */

static void ble_hid_event_cb(uint8_t evt)
{
    switch (evt) {
        case HID_DEV_SUSPEND_EVT:
            /* 主机请求挂起 — 可以进入低功耗模式 */
            break;

        case HID_DEV_EXIT_SUSPEND_EVT:
            /* 主机请求退出挂起 — 恢复正常工作 */
            break;

        case HID_DEV_SET_BOOT_EVT:
            /* 主机设置启动协议模式（Boot Protocol） */
            break;

        case HID_DEV_SET_REPORT_EVT:
            /* 主机设置报告协议模式（Report Protocol） */
            break;

        default:
            break;
    }
}

/* ========================================================================
 * HID 报告读写回调
 *
 * 当主机读/写 HID 报告时调用（通常是写 LED 状态：Caps Lock/Num Lock）
 * 参数：
 *   id - 报告 ID
 *   type - 报告类型（输入/输出/特性）
 *   uuid - 特征 UUID
 *   oper - 操作类型（读/写/使能通知/禁用通知）
 *   pLen - 数据长度指针
 *   pData - 数据指针
 * 返回：成功返回 SUCCESS，失败返回错误码
 * ======================================================================== */

static uint8_t ble_hid_report_cb(uint8_t id, uint8_t type, uint16_t uuid,
                                  uint8_t oper, uint16_t *pLen, uint8_t *pData)
{
    uint8_t status = SUCCESS;

    /* 写操作 — 主机写 HID 报告（通常是 LED 状态：Caps Lock/Num Lock） */
    if (oper == HID_DEV_OPER_WRITE) {
        if (uuid == REPORT_UUID) {
            /* 处理 LED 输出报告，忽略其他 */
            if (type == HID_REPORT_TYPE_OUTPUT) {
                /* 验证数据长度 */
                if (*pLen == HID_LED_OUT_RPT_LEN) {
                    status = SUCCESS;  /* 暂时不处理 LED 状态 */
                } else {
                    status = ATT_ERR_INVALID_VALUE_SIZE;
                }
            }
        }

        if (status == SUCCESS) {
            status = Hid_SetParameter(id, type, uuid, *pLen, pData);
        }
    }
    /* 读操作 — 主机读 HID 报告 */
    else if (oper == HID_DEV_OPER_READ) {
        status = Hid_GetParameter(id, type, uuid, pLen, pData);
    }
    /* 通知使能 — 主机使能通知（官方示例在这里启动自动发送报告的测试定时器，我们不需要） */
    else if (oper == HID_DEV_OPER_ENABLE) {
        /* 暂时不启动自动发送报告的定时器 */
    }

    return status;
}

/* ========================================================================
 * HID 报告发送（非阻塞重试）
 * ======================================================================== */

/* 尝试发送报告，失败时保存到待重试队列 */
static bool ble_pump_allowed = false;

void ble_qmk_task_running(bool running)
{
    ble_pump_allowed = running;
}

/* QMK blocks in wait_ms() for as long as a macro plays. In BT mode TMOS keeps running meanwhile, or the
 * link would drop after the supervision timeout. The flag stays off inside TMOS, so a callback never
 * nests another TMOS_SystemProcess(). */
void ble_wait_ms(uint32_t ms)
{
    if (!ble_pump_allowed || wired_mode_active) {
        DelayMs(ms);
        return;
    }

    uint32_t start = timer_read32();
    ble_pump_allowed = false;
    while (timer_elapsed32(start) < ms) {
        TMOS_SystemProcess();
#ifdef RGB_MATRIX_ENABLE
        extern void rgb_matrix_task(void);
        rgb_matrix_task();
#endif
    }
    ble_pump_allowed = true;
}

static bool ble_send_report_internal(uint8_t *report)
{
    if (current_state != BLE_STATE_CONNECTED) {
        return false;
    }

    uint8_t status = HidDev_Report(HID_RPT_ID_KEY_IN, HID_REPORT_TYPE_INPUT,
                                    HID_KEYBOARD_IN_RPT_LEN, report);
    return (status == SUCCESS);
}

void ble_send_keyboard_report(uint8_t modifiers, uint8_t *keys)
{
    uint8_t report[HID_KEYBOARD_IN_RPT_LEN] = {0};
    report[0] = modifiers;
    if (keys != NULL) {
        for (int i = 0; i < 6; i++) {
            report[2 + i] = keys[i];
        }
    }

    /* The stack holds only a few packets per connection event. Give it time to send them, so a macro
     * does not lose keys; a later report would overwrite the pending one. */
    uint32_t start = timer_read32();
    bool sent = ble_send_report_internal(report);
    while (!sent && ble_pump_allowed && current_state == BLE_STATE_CONNECTED &&
           timer_elapsed32(start) < BLE_SEND_WAIT_MS) {
        ble_wait_ms(1);
        sent = ble_send_report_internal(report);
    }

    if (!sent) {
        /* 发送失败，保存到待重试队列（覆盖旧的，因为新报告状态最新）*/
        memcpy(pending_report, report, HID_KEYBOARD_IN_RPT_LEN);
        pending_report_valid = true;
    } else {
        pending_report_valid = false;
    }
}

void ble_send_keyboard_release(void)
{
    uint8_t report[HID_KEYBOARD_IN_RPT_LEN] = {0};
    if (!ble_send_report_internal(report)) {
        memcpy(pending_report, report, HID_KEYBOARD_IN_RPT_LEN);
        pending_report_valid = true;
    } else {
        pending_report_valid = false;
    }
}

static bool ble_send_extra_internal(uint8_t id, uint8_t len, uint8_t *data)
{
    if (current_state != BLE_STATE_CONNECTED) {
        return false;
    }

    return HidDev_Report(id, HID_REPORT_TYPE_INPUT, len, data) == SUCCESS;
}

static void ble_send_extra(uint8_t id, uint8_t len, uint8_t *data)
{
    ble_pending_extra_t *slot = NULL;
    for (uint8_t i = 0; i < BLE_PENDING_EXTRA_COUNT; i++) {
        if (pending_extra[i].id == id) {
            slot = &pending_extra[i];
        }
    }
    if (slot == NULL || len > BLE_EXTRA_RPT_MAX_LEN) {
        return;
    }

    uint32_t start = timer_read32();
    bool sent = ble_send_extra_internal(id, len, data);
    while (!sent && ble_pump_allowed && current_state == BLE_STATE_CONNECTED &&
           timer_elapsed32(start) < BLE_SEND_WAIT_MS) {
        ble_wait_ms(1);
        sent = ble_send_extra_internal(id, len, data);
    }

    slot->valid = !sent;
    if (!sent) {
        slot->len = len;
        memcpy(slot->data, data, len);
    }
}

void ble_send_mouse_report(uint8_t *report, uint8_t len)
{
    ble_send_extra(HID_RPT_ID_MOUSE_IN, len, report);
}

void ble_send_extra_report(uint8_t report_id, uint16_t usage)
{
    uint8_t report[2] = {LO_UINT16(usage), HI_UINT16(usage)};
    ble_send_extra(report_id, sizeof(report), report);
}

/* 主循环里调用：重试待发送的报告 */
void ble_retry_pending_report(void)
{
    if (pending_report_valid && ble_send_report_internal(pending_report)) {
        pending_report_valid = false;
    }

    for (uint8_t i = 0; i < BLE_PENDING_EXTRA_COUNT; i++) {
        ble_pending_extra_t *slot = &pending_extra[i];
        if (slot->valid && ble_send_extra_internal(slot->id, slot->len, slot->data)) {
            slot->valid = false;
        }
    }
}

/* ========================================================================
 * 状态查询和控制
 * ======================================================================== */

ble_state_t ble_get_state(void)
{
    return current_state;
}

bool ble_is_connected(void)
{
    return (current_state == BLE_STATE_CONNECTED);
}

uint16_t ble_get_conn_handle(void)
{
    return conn_handle;
}

void ble_start_advertising(void)
{
    if (current_state != BLE_STATE_CONNECTED && !wired_mode_active) {
        ble_set_pairing_allowed(true);
        uint8_t advertising_enable = TRUE;
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
        adv_active = true;
        current_state = BLE_STATE_ADVERTISING;
        led_set_conn_state(LED_CONN_PAIRING);
    }
}

void ble_disconnect(void)
{
    /* 只判断连接句柄是否有效 */
    if (conn_handle != GAP_CONNHANDLE_INIT) {
        GAPRole_TerminateLink(conn_handle);
    }
}

/* ========================================================================
 * 三设备切换
 * The table holds 3 slots (kept in data flash). Tap: switch/reconnect, hold: pair, BT_CLEAR: clear.
 * ======================================================================== */

/* ------------------------------------------------------------------------
 * Bond table in its own data flash page
 * ------------------------------------------------------------------------ */

static void bond_table_set_empty(void)
{
    memset(&bond_table, 0, sizeof(ble_bond_table_t));
    bond_table.magic = BLE_BOND_TABLE_MAGIC;
    bond_table.layout = BLE_BOND_TABLE_LAYOUT;
    bond_table.current_device = 1;
}

/* Load the table; first use or bad contents give an empty table. A table from an older layout (or none)
 * also erases the whole SNV area, which removes our bonds for the old addresses and anything the stock
 * firmware left there, then resets once: the stack read the SNV area at init, so it must start again
 * on the clean area. */
static void bond_table_load(void)
{
    if (EEPROM_READ(BLE_BOND_TABLE_FLASH_ADDR, &bond_table, sizeof(ble_bond_table_t)) != 0 ||
        bond_table.magic != BLE_BOND_TABLE_MAGIC || bond_table.layout != BLE_BOND_TABLE_LAYOUT ||
        bond_table.current_device < 1 || bond_table.current_device > BLE_MAX_DEVICES) {
        EEPROM_ERASE(BLE_SNV_ADDR, BLE_SNV_BLOCK * BLE_SNV_NUM);
        bond_table_set_empty();
        bond_table_save();

        /* reset only if the next boot will read the new table back, so this cannot loop */
        ble_bond_table_t stored __attribute__((aligned(4)));
        if (EEPROM_READ(BLE_BOND_TABLE_FLASH_ADDR, &stored, sizeof(ble_bond_table_t)) == 0 &&
            memcmp(&stored, &bond_table, sizeof(ble_bond_table_t)) == 0) {
            SYS_ResetExecute();
        }
    }
}

/* Write the table if the page holds something else; blocks for the page erase and write */
static void bond_table_save(void)
{
    ble_bond_table_t stored __attribute__((aligned(4)));

    bond_table_dirty = false;
    if (EEPROM_READ(BLE_BOND_TABLE_FLASH_ADDR, &stored, sizeof(ble_bond_table_t)) == 0 &&
        memcmp(&stored, &bond_table, sizeof(ble_bond_table_t)) == 0) {
        return;
    }
    if (EEPROM_ERASE(BLE_BOND_TABLE_FLASH_ADDR, EEPROM_PAGE_SIZE) != 0 ||
        EEPROM_WRITE(BLE_BOND_TABLE_FLASH_ADDR, &bond_table, sizeof(ble_bond_table_t)) != 0) {
        /* try again later from ble_app_poll() */
        bond_table_dirty = true;
        bond_table_dirty_time = timer_read32();
    }
}

/* Save later from ble_app_poll(): callers run during connection setup and pairing, where blocking on
 * flash could cost connection events. */
static void bond_table_mark_dirty(void)
{
    bond_table_dirty = true;
    bond_table_dirty_time = timer_read32();
}

/* 检查指定槽位是否已配对 */
static bool bond_table_is_paired(uint8_t device_idx)
{
    if (device_idx < 1 || device_idx > BLE_MAX_DEVICES) return false;
    return bond_table.devices[device_idx - 1].paired != 0;
}

/* Store the peer in a slot; nothing happens if it is already there */
static void bond_table_set_device(uint8_t device_idx, uint8_t addr_type, uint8_t *bd_addr)
{
    if (device_idx < 1 || device_idx > BLE_MAX_DEVICES) return;
    ble_device_info_t *dev = &bond_table.devices[device_idx - 1];
    if (dev->paired && dev->addr_type == addr_type && memcmp(dev->bd_addr, bd_addr, 6) == 0) {
        return;
    }
    dev->addr_type = addr_type;
    memcpy(dev->bd_addr, bd_addr, 6);
    dev->paired = 1;
    bond_table_mark_dirty();
}

/* Called from hiddev.c on pairing progress. A bonded host reconnecting reports BONDED, a new one
 * COMPLETE and then BONDED. */
void ble_app_pair_state(uint8_t state, uint8_t status)
{
    if (status != SUCCESS || !conn_peer_valid) {
        return;
    }
    if (state == GAPBOND_PAIRING_STATE_BONDED ||
        (state == GAPBOND_PAIRING_STATE_COMPLETE && link_may_pair)) {
        link_secure = true;
        bond_update_pending = true;
        link_secure_time = timer_read32();
        /* A bonded host may come back with a new private address: keep the latest one */
        bond_table_set_device(bond_table.current_device, conn_peer_addr_type, conn_peer_addr);
    }
}

/* Pairing mode of the bond manager. Only a pairing hold or an empty slot accepts a pairing request; a
 * host that is already bonded only encrypts with its key and is not affected. */
static void ble_set_pairing_allowed(bool allowed)
{
    uint8_t mode = allowed ? BLE_PAIRING_MODE : GAPBOND_PAIRING_MODE_NO_PAIRING;
    GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &mode);
}

/* Clear all slots. Saved at once, because a factory reset follows immediately. */
static void bond_table_clear_all(void)
{
    bond_table_set_empty();
    bond_table.snv_erase_pending = 1;
    bond_table_save();
}

/* Erase the stack's bonds if that is pending. Only possible with the stack up and no link, so this is
 * also called from GAPROLE_STARTED and GAPROLE_WAITING; the pending flag survives a reset. */
static void ble_erase_stack_bonds(void)
{
    if (!bond_table.snv_erase_pending || !stack_started || conn_handle != GAP_CONNHANDLE_INIT) {
        return;
    }
    if (GAPBondMgr_SetParameter(GAPBOND_ERASE_ALLBONDS, 0, NULL) == SUCCESS) {
        bond_table.snv_erase_pending = 0;
        bond_table_mark_dirty();
    }
}

/* ------------------------------------------------------------------------
 * 定向广播 — 回连已配对主机
 * ------------------------------------------------------------------------ */

/* 开始定向广播（回连模式），只向指定 BD_ADDR 的设备广播 */
static void ble_start_directed_advertising(uint8_t addr_type, uint8_t *bd_addr)
{
    /* 设置定向广播的目标地址 */
    GAPRole_SetParameter(GAPROLE_ADV_DIRECT_TYPE, sizeof(uint8_t), &addr_type);
    GAPRole_SetParameter(GAPROLE_ADV_DIRECT_ADDR, 6, bd_addr);

    /* 设置广播类型为定向广播 */
    uint8_t adv_event_type = GAP_ADRPT_ADV_DIRECT_IND;  /* 定向可连接广播 */
    GAPRole_SetParameter(GAPROLE_ADV_EVENT_TYPE, sizeof(uint8_t), &adv_event_type);

    /* 开始广播 */
    uint8_t advertising_enable = TRUE;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
    adv_active = true;

    /* state_enter_time is set by ble_run_pending(): the 30 s cover all reconnect phases */
    current_state = BLE_STATE_RECONNECTING;
    led_set_conn_state(LED_CONN_RECONNECTING);  /* 慢闪表示回连中 */
}

/* Reconnect to the current slot. A host with a resolvable private address (random, top bits 01) has
 * changed it since it was stored, so directed advertising would only waste its 1.28 s: general
 * advertising at once. Pairing is closed either way (ble_run_pending()). */
static void ble_start_reconnect(void)
{
    ble_device_info_t *dev = &bond_table.devices[bond_table.current_device - 1];

    if (dev->addr_type != ADDRTYPE_PUBLIC && (dev->bd_addr[B_ADDR_LEN - 1] & 0xC0) == 0x40) {
        ble_start_general_advertising();
        current_state = BLE_STATE_RECONNECTING;
        led_set_conn_state(LED_CONN_RECONNECTING);
    } else {
        ble_start_directed_advertising(dev->addr_type, dev->bd_addr);
    }
}

/* 开始普通广播（可被新设备搜索到） */
static void ble_start_general_advertising(void)
{
    /* 恢复普通广播类型 */
    uint8_t adv_event_type = GAP_ADRPT_ADV_IND;  /* 普通可连接广播 */
    GAPRole_SetParameter(GAPROLE_ADV_EVENT_TYPE, sizeof(uint8_t), &adv_event_type);

    /* 清除定向广播地址 */
    uint8_t zero_addr[6] = {0};
    GAPRole_SetParameter(GAPROLE_ADV_DIRECT_ADDR, 6, zero_addr);

    /* 开始广播 */
    uint8_t advertising_enable = TRUE;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
    adv_active = true;
}

/* ------------------------------------------------------------------------
 * Timeouts, called from ble_app_poll()
 * ------------------------------------------------------------------------ */

static void ble_check_timeout(void)
{
    uint32_t now = timer_read32();

    if (wired_mode_active) {
        return;
    }

    if (current_state == BLE_STATE_CONNECTED && bond_update_pending &&
        now - link_secure_time > BLE_BOND_UPDATE_DELAY_MS) {
        bond_update_pending = false;
        GAPBondMgr_SetParameter(GAPBOND_BOND_UPDATE, 0, NULL);
    }

    if (current_state == BLE_STATE_CONNECTED && !link_may_pair && !link_secure &&
        now - link_start_time > BLE_ENCRYPT_TIMEOUT_MS) {
        /* not this slot's host: drop it, GAPROLE_WAITING reconnects the slot */
        link_start_time = now;
        ble_disconnect();
    } else if (current_state == BLE_STATE_RECONNECTING) {
        /* 30 s total (directed, then general advertising), then standby: a key press restarts it */
        if (now - state_enter_time > BLE_RECONNECT_TIMEOUT) {
            ble_stop_advertising();
            current_state = BLE_STATE_STANDBY;
            led_set_conn_state(LED_CONN_IDLE);
        }
    } else if (current_state == BLE_STATE_PAIRING) {
        /* Pairing timeout: back to the old bond of this slot if it has one, else standby */
        if (now - state_enter_time > BLE_PAIRING_TIMEOUT) {
            if (bond_table_is_paired(bond_table.current_device)) {
                ble_request_slot(bond_table.current_device, false);
            } else {
                ble_stop_advertising();
                current_state = BLE_STATE_STANDBY;
                led_set_conn_state(LED_CONN_IDLE);
            }
        }
    } else if (current_state == BLE_STATE_SWITCHING &&
               (pending_switch_device != 0 || pending_pair_device != 0)) {
        /* The stack does not report the end of advertising that had not really started yet, and it
         * can refuse the address: try again from here. */
        if (conn_handle == GAP_CONNHANDLE_INIT && stack_started && now - state_enter_time > BLE_SWITCH_RETRY_MS) {
            adv_active = false;
            ble_run_pending();
        }
    }
}

void ble_stop_advertising(void)
{
    uint8_t advertising_enable = FALSE;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
    if (current_state == BLE_STATE_ADVERTISING ||
        current_state == BLE_STATE_RECONNECTING ||
        current_state == BLE_STATE_PAIRING) {
        current_state = BLE_STATE_IDLE;
    }
}

/* Wired mode: BLE off. Called with wired_mode_active already set; a link still going down ends in
 * GAPROLE_WAITING, which sets IDLE. */
void ble_radio_off(void)
{
    pending_switch_device = 0;
    pending_pair_device = 0;
    switch_retries = 0;

    ble_disconnect();
    ble_stop_advertising();
    if (conn_handle == GAP_CONNHANDLE_INIT) {
        current_state = BLE_STATE_IDLE;
    }
}

/* Start what is pending. Must only run with no link and no advertising active. On a refused address
 * the target stays pending and ble_check_timeout() tries again. */
static void ble_run_pending(void)
{
    uint8_t device_idx = pending_switch_device != 0 ? pending_switch_device : pending_pair_device;
    bool pairing = pending_switch_device == 0;

    if (device_idx == 0) {
        return;
    }

    ble_set_device_name(device_idx);
    state_enter_time = timer_read32();

    if (!ble_set_device_address(device_idx)) {
        current_state = BLE_STATE_SWITCHING;
        if (++switch_retries > BLE_SWITCH_MAX_RETRIES) {
            /* give up: the status LED shows the failure, a key press tries again */
            pending_switch_device = 0;
            pending_pair_device = 0;
            switch_retries = 0;
            current_state = BLE_STATE_STANDBY;
        }
        return;
    }

    pending_switch_device = 0;
    pending_pair_device = 0;
    switch_retries = 0;

    if (!pairing && bond_table_is_paired(device_idx)) {
        /* directed advertising first (unless the host uses a private address); general advertising
         * follows when it times out. No pairing: only the bonded host may connect. */
        ble_set_pairing_allowed(false);
        ble_start_reconnect();
    } else {
        /* no bond in this slot, or a pairing hold: open for pairing (an old bond stays until a new
         * pairing replaces it) */
        ble_set_pairing_allowed(true);
        current_state = BLE_STATE_PAIRING;
        ble_start_general_advertising();
        led_set_conn_state(LED_CONN_PAIRING);
    }
}

/* Go to a slot. The address can only change with no link and no advertising, and starting advertising
 * while it is still ending would be lost, so wait for GAPROLE_WAITING if needed. Before GAPROLE_STARTED
 * the target just stays pending. */
static void ble_request_slot(uint8_t device_idx, bool pairing)
{
    pending_switch_device = pairing ? 0 : device_idx;
    pending_pair_device = pairing ? device_idx : 0;
    switch_retries = 0;

    if (conn_handle != GAP_CONNHANDLE_INIT) {
        current_state = BLE_STATE_SWITCHING;
        state_enter_time = timer_read32();
        ble_disconnect();
    } else if (adv_active) {
        uint8_t advertising_enable = FALSE;
        current_state = BLE_STATE_SWITCHING;
        state_enter_time = timer_read32();
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
    } else if (stack_started) {
        ble_run_pending();
    }
}

static void ble_select_slot(uint8_t device_idx)
{
    if (bond_table.current_device != device_idx) {
        bond_table.current_device = device_idx;
        bond_table_mark_dirty();
    }
}

/* BT keys and the slide switch; does nothing in the USB position */
void ble_switch_device(uint8_t device_idx)
{
    if (device_idx < 1 || device_idx > BLE_MAX_DEVICES || wired_mode_active) {
        return;
    }

    ble_select_slot(device_idx);
    ble_request_slot(device_idx, false);
}

/* Pairing hold on a BT key; does nothing in the USB position. The old bond is not cleared: a new pairing
 * replaces it, a timeout keeps it. */
void ble_pair_device(uint8_t device_idx)
{
    if (device_idx < 1 || device_idx > BLE_MAX_DEVICES || wired_mode_active) {
        return;
    }

    ble_select_slot(device_idx);
    ble_request_slot(device_idx, true);
}

/* Key press: after a timeout this restarts the current slot */
void ble_wake_from_standby(void)
{
    if (!wired_mode_active && stack_started &&
        (current_state == BLE_STATE_STANDBY || current_state == BLE_STATE_IDLE)) {
        ble_switch_device(bond_table.current_device);
    }
}

/* Clear all slots and the stack's bonds. The stack erases its bonds only without a link, so with a
 * link this happens in GAPROLE_WAITING. In BT mode slot 1, now without a bond, then opens for pairing;
 * in the USB position the radio stays off. */
void ble_clear_bonds(void)
{
    bond_table_clear_all();
    ble_erase_stack_bonds();
    ble_set_device_name(1);
    ble_switch_device(1);
}

/* ========================================================================
 * Slot memory: the selected slot is kept in the bond table. The mode is not
 * stored, the slide switch decides it.
 * ======================================================================== */

uint8_t ble_get_current_device(void)
{
    return bond_table.current_device;
}

/* Called from main after init. Only restores the slot: the stack is not up yet, so the GAPROLE_STARTED
 * case starts it, if the board is in BT mode. */
void ble_restore_last_mode(void)
{
    ble_set_device_name(bond_table.current_device);
    if (stack_started && !wired_mode_active) {
        ble_switch_device(bond_table.current_device);
    }
}

/* ========================================================================
 * Main loop part, called from housekeeping. TMOS itself runs in platform_run().
 * ======================================================================== */

void ble_app_poll(void)
{
    ble_check_timeout();

    if (bond_table_dirty && timer_read32() - bond_table_dirty_time > BLE_BOND_SAVE_DELAY_MS) {
        bond_table_save();
    }
}

/* ========================================================================
 * 连接间隔动态调整
 * 调用 GAPRole_PeripheralConnParamUpdateReq() 发起请求，主机接受后生效
 * ======================================================================== */

/* 防抖最小间隔：两次请求至少间隔5秒 */
#define PARAM_UPDATE_MIN_INTERVAL_MS  5000

/* 核心：发送连接间隔更新请求 */
static void ble_send_conn_interval_req(uint16_t min_interval, uint16_t max_interval)
{
    if (current_state != BLE_STATE_CONNECTED) return;
    GAPRole_PeripheralConnParamUpdateReq(conn_handle,
        min_interval, max_interval,
        min_interval == BLE_CONN_INTERVAL_L0_MIN ? BLE_SLAVE_LATENCY : 0, BLE_CONN_TIMEOUT,
        ble_task_id);
    last_param_update_time = timer_read32();
}

static void ble_send_conn_level_req(uint8_t level)
{
    switch (level) {
        case 0: ble_send_conn_interval_req(BLE_CONN_INTERVAL_L0_MIN, BLE_CONN_INTERVAL_L0_MAX); break;
        default: ble_send_conn_interval_req(BLE_CONN_INTERVAL_L1_MIN, BLE_CONN_INTERVAL_L1_MAX); break;
    }
    current_interval_level = level;
}

/* 请求切换连接间隔档位（带5秒防抖）*/
void ble_request_conn_interval(uint8_t level)
{
    if (fixed_conn_interval != 0) return;
    if (level >= BLE_CONN_INTERVAL_LEVELS) return;
    if (level == current_interval_level) return;

    if (current_state != BLE_STATE_CONNECTED) {
        current_interval_level = level;
        return;
    }

    /* 防抖：5秒内不重复请求 */
    uint32_t now = timer_read32();
    if (last_param_update_time != 0 &&
        (now - last_param_update_time) < PARAM_UPDATE_MIN_INTERVAL_MS) {
        return;
    }

    ble_send_conn_level_req(level);
}

/* 强制切换连接间隔档位（绕过5秒防抖，用于携带模式切换）*/
void ble_request_conn_interval_force(uint8_t level)
{
    if (level >= BLE_CONN_INTERVAL_LEVELS) return;
    if (level == current_interval_level) return;

    ble_send_conn_level_req(level);
}

/* 直接设置连接间隔（单位1.25ms，用于携带模式2000ms=1600）*/
void ble_set_conn_interval_raw(uint16_t interval)
{
    ble_send_conn_interval_req(interval, interval);
    current_interval_level = 0xFF; /* 标记为非标准档位 */
}

/* Mobile mode: sent now if connected, and again after every reconnect (START_PARAM_UPDATE_EVT) */
void ble_set_fixed_conn_interval(uint16_t interval)
{
    fixed_conn_interval = interval;
    if (interval != 0) {
        ble_set_conn_interval_raw(interval);
    } else {
        ble_request_conn_interval_force(0);
    }
}

/* Requested level, 0xFF for a raw or fixed interval */
uint8_t ble_get_current_interval_level(void)
{
    return current_interval_level;
}
