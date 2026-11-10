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
/* At or above this (smoothed) the charger is taken to be running: it holds the cell at 4.2 V (PA6 read
 * 4.18-4.19 V with the cable in, phase 40c), a cell on its own rests below. A full cell with the charger
 * done reads the same with or without the cable, so this only sees a cable while the battery charges. */
#define BATTERY_CHARGING_VOLTAGE_MV  4150
/* A full cell resting after the charge stays above that level for a while: once the voltage has fallen this
 * far below its peak, it no longer counts as charging */
#define BATTERY_CHARGING_DROP_MV     20

void     battery_init(void);
uint8_t  battery_get_level(void);       /* 0-100 %, 100 while charging */
uint16_t battery_get_voltage_mv(void);  /* smoothed battery voltage as measured, LED load included */
bool     battery_is_critical(void);     /* below BATTERY_SHUTDOWN_VOLTAGE_MV */
bool     battery_is_charging(void);     /* at the charging level and not falling */

#endif /* __BATTERY_H */
