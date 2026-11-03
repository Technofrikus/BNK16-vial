/*
 * battery.h — battery voltage and level (PA6 = AIN10, 1:2 divider, see battery.c)
 */

#ifndef __BATTERY_H
#define __BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/* 0 % here. The board still runs at 3.0 V (tested), but the cell has little left below this. */
#define BATTERY_EMPTY_VOLTAGE_MV     3300
/* Below this (smoothed) the board shuts down on battery to protect the cell; a key reboots it */
#define BATTERY_SHUTDOWN_VOLTAGE_MV  3200

void     battery_init(void);
uint8_t  battery_get_level(void);       /* 0-100 %, 100 while charging */
uint16_t battery_get_voltage_mv(void);  /* smoothed battery voltage */
bool     battery_is_critical(void);     /* below BATTERY_SHUTDOWN_VOLTAGE_MV */
bool     battery_is_charging(void);     /* always false: no charger status pin */
bool     battery_is_battery_powered(void); /* always false: no VBUS pin */

#endif /* __BATTERY_H */
