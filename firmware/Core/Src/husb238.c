#include "husb238.h"
#include <stdio.h>

// 器件说明书(en)：https://www.hynetek.com/uploadfiles/site/219/news/5cbded03-5b83-4b55-9142-e7c917e413db.pdf
// 器件说明书(zh)：https://www.hynetek.com/uploadfiles/site/219/news/a897dd88-6400-46cd-ac21-2d85f0ae638f.pdf
// 寄存器说明(en)：https://www.hynetek.com/uploadfiles/site/219/news/c3afa59e-1c03-4b92-8525-873c152bae4b.pdf

/*
 * 调试日志开关：
 *  1 = 输出详细日志（上电/能力/切档，方便调试，占更多 FLASH）
 *  0 = 仅保留错误日志（I2C 失败、切档失败），固件更小
 */
#ifndef HUSB238_DEBUG
#define HUSB238_DEBUG 1
#endif

#if HUSB238_DEBUG
#define PD_LOG(...) printf(__VA_ARGS__)
#else
#define PD_LOG(...)
#endif

bool pd_available = false;
volatile bool pd_caps_updated = false;   // 能力数据有变化（供应用层重绘灯光）
uint8_t pd_status0 = 0;
uint8_t pd_status1 = 0;

/* 各电压档能力列表（索引 0..5 对应 5V..20V） */
PD_t pd_list[PD_COUNT] = {};

/* 当前协商电压缓存（由 husb238_LoadStatus 周期刷新，避免频繁 I2C 读取） */
static PD_Level_t current_level = PD_Unvailable;

/* 上一次的能力快照（用于检测变化） */
static uint8_t last_caps[PD_COUNT] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void husb238_Write(uint8_t reg, uint8_t *data, uint8_t len) {
  HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&hi2c1, HUSB238_I2C_ADDR << 1, reg, I2C_MEMADD_SIZE_8BIT,
                                           data, len, 100);
  if (st != HAL_OK) {
    printf("[HUSB238] I2C write fail reg=0x%02X st=%d \n", reg, st);
  }
}

void husb238_Read(uint8_t reg, uint8_t *data, uint8_t len) {
  HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1, HUSB238_I2C_ADDR << 1 | 1, reg, I2C_MEMADD_SIZE_8BIT,
                                          data, len, 100);
  if (st != HAL_OK) {
    printf("[HUSB238] I2C read fail reg=0x%02X st=%d \n", reg, st);
  }
}

char* husb238_LevelToChar(PD_Level_t lv)
{
  switch (lv)
  {
    case PD_5V:
      return "5V";
    case PD_9V:
      return "9V";
    case PD_12V:
      return "12V";
    case PD_15V:
      return "15V";
    case PD_18V:
      return "18V";
    case PD_20V:
      return "20V";
    default:
      return "N/A";
  }
}

// 电流码（低4位）转 mA
uint16_t husb238_ReadCurrent(uint8_t reg)
{
  switch (reg & 0b1111)
  {
  case 0b0000: return 500;    // 0.5A
  case 0b0001: return 700;    // 0.7A
  case 0b0010: return 1000;   // 1.0A
  case 0b0011: return 1250;   // 1.25A
  case 0b0100: return 1500;   // 1.5A
  case 0b0101: return 1750;   // 1.75A
  case 0b0110: return 2000;   // 2.0A
  case 0b0111: return 2250;   // 2.25A
  case 0b1000: return 2500;   // 2.5A
  case 0b1001: return 2750;   // 2.75A
  case 0b1010: return 3000;   // 3.0A
  case 0b1011: return 3250;   // 3.25A
  case 0b1100: return 3500;   // 3.5A
  case 0b1101: return 4000;   // 4.0A
  case 0b1110: return 4500;   // 4.5A
  case 0b1111: return 5000;   // 5.0A
  default:     return 0;
  }
}

// 打印单个档位信息（供调试）
void husb238_PrintPD(PD_t pd)
{
  printf("       %3s, available: %d, current: %umA \n",
         husb238_LevelToChar(pd.level), pd.available, pd.current);
}

// 读取各电压档能力（寄存器 0x02..0x07 = SRC_PDO_5V..20V，bit7=检测到，低4位=电流）
void husb238_LoadReg(bool verbose)
{
  uint8_t regs[PD_COUNT] = {0};
  HAL_I2C_Mem_Read(&hi2c1, HUSB238_I2C_ADDR << 1 | 1, 0x02, I2C_MEMADD_SIZE_8BIT,
                   regs, PD_COUNT, 100);

  // 解析能力列表
  for (uint8_t i = 0; i < PD_COUNT; i++) {
    pd_list[i].level     = (PD_Level_t)(i + 1);          // 5V..20V
    pd_list[i].available = (regs[i] & 0x80) != 0;        // bit7：检测到
    pd_list[i].current   = husb238_ReadCurrent(regs[i]); // 低4位：档位最大电流
  }

  // 与上次快照比较，有变化则通知应用层
  bool changed = false;
  for (uint8_t i = 0; i < PD_COUNT; i++) {
    uint8_t v = (pd_list[i].available ? 0x80 : 0) | (uint8_t)(pd_list[i].current / 100);
    if (v != last_caps[i]) {
      last_caps[i] = v;
      changed = true;
    }
  }
  if (changed) {
    pd_caps_updated = true;
  }

  if (verbose) {
    PD_LOG("[HUSB238] pd_list: \n");
    for (int i = 0; i < PD_COUNT; i++) {
      husb238_PrintPD(pd_list[i]);
    }
  }
}

// 读取协商状态并刷新电压缓存
void husb238_LoadStatus(void) {
  uint8_t reg_val[2] = {0};
  husb238_Read(HUSB238_REG_PD_STATUS0, reg_val, 2);

  pd_status0 = reg_val[0];
  pd_status1 = reg_val[1];

  pd_available = pd_status0 > 0 || pd_status1 > 0;

  // PD_STATUS0 高4位：当前协商电压编码（0001=5V ... 0110=20V）
  switch (pd_status0 >> 4) {
    case 0b0001: current_level = PD_5V;  break;
    case 0b0010: current_level = PD_9V;  break;
    case 0b0011: current_level = PD_12V; break;
    case 0b0100: current_level = PD_15V; break;
    case 0b0101: current_level = PD_18V; break;
    case 0b0110: current_level = PD_20V; break;
    default:     current_level = PD_Unvailable; break;
  }
}

void Husb238_Init(void) {
  PD_LOG("[HUSB238] Init \n");

  // 触发获取适配器能力（Get_SRC_Cap）
  PD_LOG("[HUSB238] Trigger Get Adapter Capability \n");
  uint8_t reg_val = 0b00100; // 00100 = Send out Get_SRC_Cap command
  husb238_Write(HUSB238_REG_GO_COMMAND, &reg_val, 1);

  // 等待 HUSB238 完成上电协商（最多 2s），确保切档前已稳定
  for (uint8_t i = 0; i < 20; i++) {
    HAL_Delay(100);
    husb238_LoadStatus();
    if (pd_available) break;
  }

  // 上电默认 5V（SetLevel 内部已是 5V 时直接跳过，不触发切换、不掉电）
  if (Husb238_SetLevel(PD_5V)) {
    PD_LOG("[HUSB238] SetLevel(5V) success \n");
  } else {
    printf("[HUSB238] SetLevel(5V) failed, retry at boot flow end \n");
  }

  // 无条件读取能力（避免 pd_list 为空导致按键调档失效）
  husb238_LoadReg(true);
}

// 重新获取适配器能力（流水灯结束后调用，确保数据最新）
void Husb238_RefreshCaps(void) {
  uint8_t reg_val = 0b00100; // Send out Get_SRC_Cap command
  husb238_Write(HUSB238_REG_GO_COMMAND, &reg_val, 1);

  HAL_Delay(100);

  husb238_LoadReg(true);
}

/* 上次状态刷新计时（每 10ms 由 Husb238_Pre10ms 累加） */
uint32_t pd_reload_status_time_ms = 0;

void Husb238_Pre10ms(void)
{
  // 每 10ms 调用一次，累加计时，每 5s 刷新一次协商状态（闲置时同步）
  pd_reload_status_time_ms += 10;
  if (pd_reload_status_time_ms >= PD_RELOAD_STATUS_TIME_MS) {
    pd_reload_status_time_ms = 0;
    husb238_LoadStatus();
  }
}

// 返回当前协商电压（读缓存，不触发 I2C）
PD_Level_t Husb238_GetLevel(void) {
  return current_level;
}

PD_Level_t Husb238_NextLevel(void)
{
  PD_Level_t lv = current_level;
  if (!lv) return PD_Unvailable;
  if (lv == PD_20V) return PD_Unvailable;

  // 跳过不支持的档位，返回下一个支持的档
  for (uint8_t i = lv + 1; i < PD_COUNT + 1; i++)
  {
    PD_t pd = pd_list[i - 1];
    if (pd.available && pd.level != PD_Unvailable)
    {
      return pd.level;
    }
  }

  return PD_Unvailable;
}

PD_Level_t Husb238_PrevLevel(void)
{
  PD_Level_t lv = current_level;
  if (!lv) return PD_Unvailable;
  if (lv == PD_5V) return PD_Unvailable;

  // 跳过不支持的档位，返回上一个支持的档
  for (uint8_t i = lv - 1; i > 0; i--)
  {
    PD_t pd = pd_list[i - 1];
    if (pd.available)
    {
      return pd.level;
    }
  }

  return PD_Unvailable;
}

bool Husb238_LevelAvailable(PD_Level_t lv) {
  if (lv == PD_Unvailable || lv > PD_20V) return false;
  return pd_list[lv - 1].available;
}

bool Husb238_SetLevel(PD_Level_t lv) {
  // 已是目标电压则直接返回成功：
  // 避免触发无意义的重新协商——HUSB238 切换电压时 VBUS 会瞬断，
  // 若 MCU 从 VBUS 取电且储能不足会掉电复位，导致上电反复重启
  if (current_level == lv) {
    return true;
  }

  // 设置目标电压（PDO_SELECT 编码）
  uint8_t reg_val = 0;
  switch (lv) {
  case PD_5V:
    reg_val = 0b0001;
    break;
  case PD_9V:
    reg_val = 0b0010;
    break;
  case PD_12V:
    reg_val = 0b0011;
    break;
  case PD_15V:
    reg_val = 0b1000;
    break;
  case PD_18V:
    reg_val = 0b1001;
    break;
  case PD_20V:
    reg_val = 0b1010;
    break;
  }
  if (reg_val == 0) return false;
  reg_val = reg_val << 4;

  PD_LOG("[HUSB238] Set Level %s \n", husb238_LevelToChar(lv));
  husb238_Write(HUSB238_REG_SRC_PDO, &reg_val, 1);

  // 应用设置
  reg_val = 0b00001;
  husb238_Write(HUSB238_REG_GO_COMMAND, &reg_val, 1);

  // 等待电压协商完成（PD 切换需要时间）
  HAL_Delay(100);

  // 刷新状态并验证是否切到目标电压
  husb238_LoadStatus();
  return current_level == lv;
}

/* 上次能力同步时间戳 */
static uint32_t last_caps_ms = 0;

void Husb238_Task(void)
{
  uint32_t now = HAL_GetTick();

  // 每 30s 同步一次档位能力（闲置时刷新）
  if (now - last_caps_ms >= PD_RELOAD_CAPS_TIME_MS) {
    last_caps_ms = now;
    husb238_LoadReg(false);
  }
}
