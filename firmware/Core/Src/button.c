#include "button.h"

#include "main.h"
#include <stdio.h>

volatile uint8_t button_idx = 0;

// 外部事件通知：发生按键按下
void Button_OnPressed(uint16_t GPIO_Pin) {
  for (int i = 0; i < BTN_COUNT; i++) {
    Button_t* btn = button_list[i];
    if (btn->GPIO_Pin == GPIO_Pin) {
      uint32_t current_time = HAL_GetTick();

      // 消抖处理：忽略短时间内的重复中断
      if ((current_time - btn->last_interrupt_time) < BUTTON_DEBOUNCE_TIME_MS) {
        return;
      }
      btn->last_interrupt_time = current_time;

      switch (btn->state)
      {
        case BTN_STATE_IDLE:
          // 新的按键序列开始，记录按下时间
          btn->pressed_time = current_time;
          btn->first_release_time = 0;
          btn->state = BTN_STATE_PRESSED;
          break;

        case BTN_STATE_SHORT_RELEASED:
        case BTN_STATE_WAITING_FOR_SECOND_PRESS:
          // 在双击窗口期内检测到第二次按下，重置按下时间
          btn->pressed_time = current_time;
          btn->state = BTN_STATE_SECOND_PRESS_DETECTED;
          break;

        case BTN_STATE_LONG_PRESS_TRIGGERED:
          // 长按后的再次按下，重置状态机
          btn->pressed_time = current_time;
          btn->first_release_time = 0;
          btn->state = BTN_STATE_PRESSED;
          break;

        case BTN_STATE_PRESSED:
          // 重复按下（可能是抖动或异常），更新按下时间
          btn->pressed_time = current_time;
          break;

        case BTN_STATE_SECOND_PRESS_DETECTED:
          // 第二次按下状态的重复按下，更新按下时间
          btn->pressed_time = current_time;
          break;

        default:
          // 其他状态下的意外按下，重置为按下状态
          btn->pressed_time = current_time;
          btn->first_release_time = 0;
          btn->state = BTN_STATE_PRESSED;
          break;
      }
    }
  }
}


// 外部事件通知：发生按键释放
void Button_OnRelease(uint16_t GPIO_Pin) {
  for (int i = 0; i < BTN_COUNT; i++) {
    Button_t* btn = button_list[i];
    if (btn->GPIO_Pin == GPIO_Pin) {
      uint32_t current_time = HAL_GetTick();

      // 消抖处理：忽略短时间内的重复中断
      if ((current_time - btn->last_interrupt_time) < BUTTON_DEBOUNCE_TIME_MS) {
        return;
      }
      btn->last_interrupt_time = current_time;

      switch (btn->state)
      {
        case BTN_STATE_PRESSED:
          // 短按释放（未达到长按阈值），进入双击等待确认状态
          btn->first_release_time = current_time;
          btn->state = BTN_STATE_SHORT_RELEASED;
          break;

        case BTN_STATE_SECOND_PRESS_DETECTED:
          // 第二次按下后释放，判断是双击还是长按
          {
            uint32_t press_duration = current_time - btn->pressed_time;
            if (press_duration < BUTTON_LONG_PRESS_TIME_MS)
            {
              // 第二次按下为短按，确认为双击事件
              Button_Handle(btn, BTN_DOUBLE_PRESS);
            }
            // 若达到长按阈值，已在 Button_Task 中处理，此处仅重置状态
            btn->state = BTN_STATE_IDLE;
          }
          break;

        case BTN_STATE_LONG_PRESS_TRIGGERED:
          // 长按事件已触发，释放时仅重置状态机
          btn->state = BTN_STATE_IDLE;
          break;

        default:
          // 其他状态下的意外释放，重置状态机
          btn->state = BTN_STATE_IDLE;
          break;
      }
    }
  }
}

// 按钮任务处理函数，在主循环中周期性调用
void Button_Task(void)
{
  Button_t* btn = button_list[button_idx];

  // 检查短按确认超时（双击窗口期）
  if (btn->state == BTN_STATE_SHORT_RELEASED)
  {
    uint32_t current_time = HAL_GetTick();
    if ((current_time - btn->first_release_time) >= BUTTON_DOUBLE_WINDOW_MS)
    {
      // 双击窗口超时，确认为单击事件
      Button_Handle(btn, BTN_SHORT_PRESS);
      btn->state = BTN_STATE_IDLE;
    }
  }

  // 检查双击等待状态超时
  else if (btn->state == BTN_STATE_WAITING_FOR_SECOND_PRESS)
  {
    uint32_t current_time = HAL_GetTick();
    if ((current_time - btn->first_release_time) >= BUTTON_DOUBLE_WINDOW_MS)
    {
      // 双击窗口超时，确认为单击事件
      Button_Handle(btn, BTN_SHORT_PRESS);
      btn->state = BTN_STATE_IDLE;
    }
  }

  // 检查第二次按下的长按检测
  else if (btn->state == BTN_STATE_SECOND_PRESS_DETECTED)
  {
    uint32_t current_time = HAL_GetTick();
    uint32_t press_duration = current_time - btn->pressed_time;

    if (press_duration >= BUTTON_LONG_PRESS_TIME_MS)
    {
      // 第二次按下达到长按阈值，触发长按事件（而非双击）
      Button_Handle(btn, BTN_LONG_PRESS);
      btn->state = BTN_STATE_LONG_PRESS_TRIGGERED;
    }
  }

  // 检查首次按下的长按超时
  else if (btn->state == BTN_STATE_PRESSED)
  {
    uint32_t current_time = HAL_GetTick();
    uint32_t press_duration = current_time - btn->pressed_time;

    if (press_duration >= BUTTON_LONG_PRESS_TIME_MS)
    {
      // 首次按下达到长按阈值，立即触发长按事件
      Button_Handle(btn, BTN_LONG_PRESS);
      btn->state = BTN_STATE_LONG_PRESS_TRIGGERED;
    }
  }

  // 轮询下一个按键
  button_idx = (button_idx + 1) % BTN_COUNT;
}
