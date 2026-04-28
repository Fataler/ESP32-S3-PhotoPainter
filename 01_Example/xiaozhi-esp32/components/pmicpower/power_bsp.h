#pragma once

#include "i2c_bsp.h"

#define AXP2101_iqr_PIN             GPIO_NUM_21
#define AXP2101_CHGLED_PIN          GPIO_NUM_3

typedef struct {
    char isCharging[32];
    char chargeStatus[45];
    char batteryVoltage[30];
    char batteryPercent[30];
} PmicRegisterConfig;

typedef struct {
    bool available;
    bool connected;
    bool charging;
    bool discharging;
    bool standby;
    bool vbus_good;
    uint8_t charge_status;
    uint16_t voltage_mv;
    int percent;
} PmicBatteryMetrics;

void Custom_PmicPortInit(I2cMasterBus *i2cbus,uint8_t dev_addr);
void Custom_PmicRegisterInit(void);
void Axp2101_isChargingTask(void *arg);
PmicRegisterConfig Custom_PmicGetBatteryInfo(void);
PmicBatteryMetrics Custom_PmicGetBatteryMetrics(void);
