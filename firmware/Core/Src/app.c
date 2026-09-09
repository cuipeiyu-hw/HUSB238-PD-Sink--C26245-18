#include "app.h"

#include "i2c.h"
#include <stdio.h>
#include "button.h"
#include "husb238.h"
#include "main.h"

/* ---------------- 应用状态机 ---------------- */
typedef enum {
  APP_STATE_BOOT_FLOW,     // 开机流水灯（左→右→左，颜色随机）
  APP_STATE_READ_CAPS,     // 流水结束后读取能力，点亮蓝/红灯
  APP_STATE_IDLE,          // 正常运行（按键可切档）
  APP_STATE_SWITCH_FLASH,  // 切档中：目标灯绿灯闪烁 3 次
} AppState_t;

static AppState_t app_state = APP_STATE_BOOT_FLOW;

/* 开机流水灯状态（每灯渐亮渐灭，颜色随机） */
#define FLOW_STEPS       100    // 每灯渐变步数（前 10 步渐亮、后 10 步渐灭）
#define FLOW_STEP_MS     1    // 每步 3ms → 每灯 90ms
#define FLOW_HOLD_MS     100   // 全灭停留时间

static uint32_t flow_last_tick = 0;  // 上一步的时间戳
static uint8_t  flow_phase = 0;      // 0=从左(5V)到右(20V), 1=从右到左
static int8_t   flow_led   = 0;      // 当前渐变的灯珠索引
static uint8_t  flow_step  = 0;      // 当前灯珠内的渐变步
static uint8_t  flow_rgb[3];         // 当前灯珠的随机色
static bool     flow_hold  = false;  // 全灭停留中
static uint32_t flow_hold_tick = 0;  // 全灭停留开始时间戳

/* 伪随机数（线性同余），用于流水灯颜色 */
static uint32_t flow_seed = 0;

static uint8_t flow_rand8(void) {
  flow_seed = flow_seed * 1103515245u + 12345u;
  return (uint8_t)(flow_seed >> 16);
}

/* 切档状态 */
static PD_Level_t switch_target = PD_Unvailable;  // 目标档位
static PD_Level_t switch_old    = PD_Unvailable;  // 切档前的旧档位
static uint8_t    switch_flash_count = 0;         // 已完成的闪烁次数
static uint8_t    switch_flash_phase = 0;         // 0=亮, 1=灭
static uint32_t   switch_flash_tick  = 0;         // 上次翻转的时间戳

/* 所有灯光统一亮度百分比（0-100），本项目用 10% 避免刺眼 */
#define LED_BRIGHTNESS 20

/* 档位枚举值 1..6 对应灯珠索引 0..5（5V..20V） */
static uint8_t level_to_led_idx(PD_Level_t lv) {
  return (uint8_t)(lv - 1);
}

/* 单灯按能力色点亮：支持=蓝，不支持=红（50% 亮度） */
static void paint_cap_led(uint8_t led_idx) {
  if (led_idx >= LED_COUNT) return;
  if (pd_list[led_idx].available) {
    set_led_color_pct(led_idx, 0, 0, 255, LED_BRIGHTNESS);  // 蓝
  } else {
    set_led_color_pct(led_idx, 255, 0, 0, LED_BRIGHTNESS);  // 红
  }
}

/* 开机初始：全部按能力色点亮（支持的蓝灯，不支持的红灯） */
static void paint_all_caps(void) {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    paint_cap_led(i);
  }
  Lights_Refresh();
}

/* 重绘全部灯光：当前档绿常亮，其余支持=蓝、不支持=红 */
static void paint_all_lights(PD_Level_t current) {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    PD_Level_t lv = (PD_Level_t)(i + 1);
    if (pd_list[i].available && lv == current) {
      set_led_color_pct(i, 0, 255, 0, LED_BRIGHTNESS);  // 当前档：绿
    } else {
      paint_cap_led(i);
    }
  }
  Lights_Refresh();
}

/* 开机流水灯：每灯快速渐亮渐灭（颜色随机），从左(5V)到右(20V)再从右流回左，最后全灭 */
static void app_boot_flow_step(void) {
  uint32_t now = HAL_GetTick();

  // 全灭停留：停留结束后进入能力获取
  if (flow_hold) {
    if (now - flow_hold_tick >= FLOW_HOLD_MS) {
      app_state = APP_STATE_READ_CAPS;
    }
    return;
  }

  if (now - flow_last_tick < FLOW_STEP_MS) return;  // 每 FLOW_STEP_MS 走一步
  flow_last_tick = now;

  // 计算当前亮度（三角波：渐亮 0→100，再渐灭 100→0），并映射到统一亮度宏
  uint8_t half = FLOW_STEPS / 2;
  uint8_t brightness;
  if (flow_step < half) {
    brightness = (uint8_t)((uint16_t)(flow_step + 1) * 100 / half);          // 渐亮
  } else {
    brightness = (uint8_t)((uint16_t)(FLOW_STEPS - flow_step) * 100 / half); // 渐灭
  }
  uint8_t percent = (uint8_t)((uint16_t)brightness * LED_BRIGHTNESS / 100);  // 峰值 = LED_BRIGHTNESS
  set_led_color_pct((uint8_t)flow_led, flow_rgb[0], flow_rgb[1], flow_rgb[2], percent);

  flow_step++;
  if (flow_step < FLOW_STEPS) return;  // 当前灯珠渐变未完成

  // 当前灯珠渐变完成：熄灭，进入下一个灯珠
  set_led_color_pct((uint8_t)flow_led, 0, 0, 0, 0);
  flow_step = 0;

  if (flow_phase == 0) {
    // 左→右：到右端后折返（右端灯不重复）
    flow_led++;
    if (flow_led >= LED_COUNT) {
      flow_phase = 1;
      flow_led = LED_COUNT - 2;
    }
  } else {
    // 右→左：流回最左端后全灭停留
    flow_led--;
    if (flow_led < 0) {
      flow_hold = true;
      flow_hold_tick = HAL_GetTick();
      return;
    }
  }

  // 生成下一个灯珠的随机颜色（伪随机，各通道独立）
  flow_rgb[0] = flow_rand8();
  flow_rgb[1] = flow_rand8();
  flow_rgb[2] = flow_rand8();
  Lights_Refresh();
}

/* 流水灯结束后：重新获取 PD 能力并点亮蓝/红 */
static void app_read_caps(void) {
  Husb238_RefreshCaps();

  // 确认默认档位为 5V（当前已是 5V 时 SetLevel 内部直接跳过，不会触发切换）
  Husb238_SetLevel(PD_5V);

  paint_all_caps();
  app_state = APP_STATE_IDLE;
}

/* 切档闪烁状态机：目标灯绿灯闪 3 次后执行切档 */
static void app_switch_flash_step(void) {
  uint32_t now = HAL_GetTick();
  if (now - switch_flash_tick < 150) return;  // 每 150ms 翻转一次亮/灭
  switch_flash_tick = now;

  uint8_t idx = level_to_led_idx(switch_target);

  if (switch_flash_phase == 0) {
    // 亮绿灯
    set_led_color_pct(idx, 0, 255, 0, LED_BRIGHTNESS);
    switch_flash_phase = 1;
  } else {
    // 灭灯（恢复能力色），计一次闪烁
    paint_cap_led(idx);
    switch_flash_phase = 0;
    switch_flash_count++;

    if (switch_flash_count >= 3) {
      // 闪烁完毕，执行切档
      bool ok = Husb238_SetLevel(switch_target);
      if (ok) {
        // 切档成功：目标灯绿灯常亮，旧档恢复蓝灯
        set_led_color_pct(idx, 0, 255, 0, LED_BRIGHTNESS);
        paint_cap_led(level_to_led_idx(switch_old));
        printf("[APP] switch to %s OK \n", husb238_LevelToChar(switch_target));
      } else {
        // 切档失败：目标灯保持能力色，旧档不变
        paint_cap_led(idx);
        printf("[APP] switch to %s FAIL \n", husb238_LevelToChar(switch_target));
      }
      Lights_Refresh();
      app_state = APP_STATE_IDLE;
    }
  }
  Lights_Refresh();
}

/* 开始切档：目标档灯绿灯闪烁 3 次后切档 */
static void app_start_switch(PD_Level_t target) {
  if (app_state != APP_STATE_IDLE) return;  // 忙碌中忽略按键
  if (target == PD_Unvailable) return;

  switch_target = target;
  switch_old = Husb238_GetLevel();  // 记录当前档
  if (switch_old == PD_Unvailable) switch_old = PD_5V;  // 读不到时按 5V 处理
  if (switch_old == switch_target) return;  // 已在目标档，无需切换

  switch_flash_count = 0;
  switch_flash_phase = 0;
  switch_flash_tick = HAL_GetTick();
  app_state = APP_STATE_SWITCH_FLASH;
}

/* 按键事件处理 */
void Button_Handle(Button_t *btn, ButtonEvent_t event) {
  if (event == BTN_SHORT_PRESS) {
    // 短按：切换档位（跳过不支持的档）
    PD_Level_t target = PD_Unvailable;
    if (btn->GPIO_Pin == P_BTN_Pin) {
      target = Husb238_NextLevel();  // 下一个支持的档
    } else if (btn->GPIO_Pin == N_BTN_Pin) {
      target = Husb238_PrevLevel();  // 上一个支持的档
    }
    if (target != PD_Unvailable) {
      app_start_switch(target);
    } else {
      printf("[APP] no available level \n");
    }
  } else if (event == BTN_LONG_PRESS) {
    // 长按：直接跳档（P 长按=20V，N 长按=5V）
    if (btn->GPIO_Pin == P_BTN_Pin) {
      app_start_switch(PD_20V);
    } else if (btn->GPIO_Pin == N_BTN_Pin) {
      app_start_switch(PD_5V);
    }
  }
}

Button_t p_button = {
  P_BTN_GPIO_Port,
  P_BTN_Pin,
};

Button_t n_button = {
  N_BTN_GPIO_Port,
  N_BTN_Pin,
};

Button_t *button_list[BTN_COUNT] = {
  &p_button,
  &n_button,
};

void App_Init(void) {
  Lights_Init();

  Husb238_Init();

  // 从开机流水灯开始（每灯渐亮渐灭，颜色随机）
  app_state = APP_STATE_BOOT_FLOW;
  flow_phase = 0;
  flow_led   = 0;
  flow_step  = 0;
  flow_hold  = false;
  flow_seed  = HAL_GetTick();  // 伪随机种子
  flow_last_tick = HAL_GetTick();
  // 第一个灯珠的随机颜色
  flow_rgb[0] = flow_rand8();
  flow_rgb[1] = flow_rand8();
  flow_rgb[2] = flow_rand8();
}

void App_Test(void)
{

}

void App_Pre10ms(void) {
  Husb238_Pre10ms();
}

void App_Task(void) {
  Button_Task();

  Lights_Task();

  switch (app_state) {
    case APP_STATE_BOOT_FLOW:
      app_boot_flow_step();
      break;

    case APP_STATE_READ_CAPS:
      app_read_caps();
      break;

    case APP_STATE_IDLE:
      // 闲置时周期同步：能力有变化才重绘灯光
      if (pd_caps_updated) {
        pd_caps_updated = false;
        PD_Level_t cur = Husb238_GetLevel();
        paint_all_lights(cur);
      }
      break;

    case APP_STATE_SWITCH_FLASH:
      app_switch_flash_step();
      break;
  }

  Husb238_Task();
}
