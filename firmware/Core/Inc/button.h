#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include "gpio.h"

// --- 定义常量 ---
#define BUTTON_DEBOUNCE_TIME_MS        50   // 消抖时间 (毫秒)
#define BUTTON_DOUBLE_WINDOW_MS        420  // 双击检测窗口（毫秒）
#define BUTTON_LONG_PRESS_TIME_MS      1200 // 长按阈值 (毫秒)

// --- 状态机变量 ---
typedef enum {
    BTN_STATE_IDLE = 0,                  // 空闲状态，等待按键按下
    BTN_STATE_PRESSED,                   // 按键已按下，记录按下时刻
    BTN_STATE_SHORT_RELEASED,            // 短按释放，等待确认（双击窗口期内）
    BTN_STATE_WAITING_FOR_SECOND_PRESS,  // 第一次短按释放后，等待第二次短按（双击窗口）
    BTN_STATE_SECOND_PRESS_DETECTED,     // 检测到第二次短按，等待其释放
    BTN_STATE_LONG_PRESS_TRIGGERED,      // 长按已触发，等待释放
    BTN_STATE_SECOND_LONG_PRESS,         // 第二次按下为长按，等待释放
} ButtonState_t;

typedef enum {
    BTN_SHORT_PRESS = 0,
    BTN_DOUBLE_PRESS,
    BTN_LONG_PRESS,
    //BTN_LONG_PRESS_RELEASE,
} ButtonEvent_t;

typedef struct
{
    GPIO_TypeDef* GPIOx;        // 端口 (如 GPIOA)
    uint16_t GPIO_Pin;          // 引脚 (如 GPIO_Pin_0)
    ButtonState_t state;        // 状态机

    uint32_t last_interrupt_time; // 上次中断时间，用于消抖
    uint32_t pressed_time;        // 按下时间戳
    uint32_t first_release_time;  // 第一次释放时间戳（用于双击窗口计算）
} Button_t;

#define BTN_COUNT 2

extern Button_t* button_list[BTN_COUNT];

void Button_OnPressed(uint16_t GPIO_Pin);
void Button_OnRelease(uint16_t GPIO_Pin);
void Button_Task(void);

// 按钮事件处理
extern void Button_Handle(Button_t* btn, ButtonEvent_t event);

#endif //BUTTON_H
