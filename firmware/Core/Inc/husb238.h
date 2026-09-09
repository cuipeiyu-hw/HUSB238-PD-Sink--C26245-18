#ifndef HUSB238_H
#define HUSB238_H

#include "main.h"
#include "i2c.h"

#define HUSB238_I2C_ADDR        0x08

#define HUSB238_REG_PD_STATUS0  0x00
#define HUSB238_REG_SRC_PDO     0x08
#define HUSB238_REG_GO_COMMAND  0x09

typedef enum {
    PD_Unvailable = 0,
    PD_5V,
    PD_9V,
    PD_12V,
    PD_15V,
    PD_18V,
    PD_20V,
} PD_Level_t;

typedef struct
{
    PD_Level_t level;
    bool available;
    uint16_t current;   // 档位最大电流 (mA)
} PD_t;


/* 协商状态刷新周期（PD_STATUS0/1） */
#define PD_RELOAD_STATUS_TIME_MS (5*1000)
/* 档位能力同步周期（SRC_PDO_5V..20V） */
#define PD_RELOAD_CAPS_TIME_MS   (30*1000)

extern bool pd_available;
/* 能力数据有变化（供应用层重绘灯光） */
extern volatile bool pd_caps_updated;

#define PD_COUNT 6

extern PD_t pd_list[PD_COUNT];

void Husb238_Init(void);
void Husb238_RefreshCaps(void);
void Husb238_Pre10ms(void);
PD_Level_t Husb238_GetLevel(void);
PD_Level_t Husb238_NextLevel(void);
PD_Level_t Husb238_PrevLevel(void);
bool Husb238_SetLevel(PD_Level_t lv);
bool Husb238_LevelAvailable(PD_Level_t lv);
void Husb238_Task(void);

char *husb238_LevelToChar(PD_Level_t lv);

/* 兼容旧拼写（Hubs238_LevelAvailable），方便驱动移植到其他工程 */
#define Hubs238_LevelAvailable Husb238_LevelAvailable

#endif //HUSB238_H
