#include "lights.h"
#include "spi.h"
#include <string.h>

/* 每个灯珠当前的 GRB 颜色（0-255），顺序为 G,R,B，与 WS2812 线上顺序一致 */
static uint8_t led_grb[LED_COUNT][3];

/* SPI(DMA) 发送缓冲：前段为数据，尾段为复位低电平 */
static uint8_t ws2812_tx_buf[WS2812_BUF_SIZE] = {0};

/* 每个 WS2812 位用 4 个 SPI 位表示（数值即 4-bit 编码，MSB 在前） */
#define WS2812_CODE_0  8    // 0b1000 : 0.25us 高 + 0.75us 低
#define WS2812_CODE_1  14   // 0b1110 : 0.75us 高 + 0.25us 低

/* DMA 发送状态 */
static volatile bool ws2812_dma_busy = false;  // 正在发送
static volatile bool ws2812_dirty    = false;  // 颜色有更新待发送

/**
 * @brief 将全部灯珠的 GRB 颜色打包进 SPI 发送缓冲
 * @note 数据区：每颗灯 12 字节；尾部的 WS2812_RESET_BYTES 保持为 0（低电平，作复位）
 *       每个 WS2812 位用 4 个 SPI 位表示："0"=0b1000, "1"=0b1110
 */
static void ws2812_build_buffer(void) {
  /* 整缓冲清零（数据区与复位区） */
  memset(ws2812_tx_buf, 0, WS2812_BUF_SIZE);

  uint16_t bitpos = 0;  // 当前写入的 SPI 位索引

  for (uint8_t i = 0; i < LED_COUNT; i++) {
    /* WS2812 线上顺序为 G、R、B */
    const uint8_t *grb = led_grb[i];
    for (uint8_t c = 0; c < 3; c++) {
      uint8_t byte = grb[c];
      for (int8_t b = 7; b >= 0; b--) {           // 颜色字节，MSB 先发
        uint8_t bit = (byte >> b) & 0x01;
        uint8_t code = bit ? WS2812_CODE_1 : WS2812_CODE_0;  // "1" / "0" 的 4-bit 编码
        for (int8_t s = 3; s >= 0; s--) {         // 4 个 SPI 位，MSB 先发
          if ((code >> s) & 0x01) {
            ws2812_tx_buf[bitpos >> 3] |= (0x80 >> (bitpos & 0x07));
          }
          bitpos++;
        }
      }
    }
  }
  /* 复位区（bitpos 之后）保持为 0，即持续低电平 */
}

/**
 * @brief 若状态有更新且 DMA 空闲，则通过 SPI+DMA 发送最新一帧
 */
void Lights_Refresh(void) {
  if (ws2812_dma_busy) {
    return;  // 正在发送，等发送完成回调后再补发
  }
  if (!ws2812_dirty) {
    return;  // 无更新
  }

  ws2812_build_buffer();
  ws2812_dma_busy = true;
  ws2812_dirty = false;

  if (HAL_SPI_Transmit_DMA(&hspi1, ws2812_tx_buf, WS2812_BUF_SIZE) != HAL_OK) {
    /* 启动失败，释放忙标志，下一轮再试 */
    ws2812_dma_busy = false;
  }
}

/**
 * @brief SPI DMA 发送完成回调（在 HAL_SPI_TxCpltCallback 中调用）
 */
void Lights_DMA_TxCpltCallback(void) {
  ws2812_dma_busy = false;
  /* 发送期间又有更新，立即补发最新状态 */
  if (ws2812_dirty) {
    Lights_Refresh();
  }
}

/**
 * @brief HAL SPI 发送完成回调（弱函数覆盖）
 * @note SPI1 专用于 WS2812，故在此直接转发到灯光模块
 */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
  if (hspi->Instance == SPI1) {
    Lights_DMA_TxCpltCallback();
  }
}

/**
 * @brief 直接设置指定 LED 的 RGB 颜色
 * @param led_idx LED 索引 (0-5)
 * @param r/g/b 颜色分量 (0-255)
 */
void set_led_color(uint8_t led_idx, uint8_t r, uint8_t g, uint8_t b) {
  if (led_idx >= LED_COUNT) return;
  led_grb[led_idx][1] = r;
  led_grb[led_idx][0] = g;
  led_grb[led_idx][2] = b;
  ws2812_dirty = true;
}

/**
 * @brief 以指定亮度百分比设置某灯 RGB 颜色
 * @param led_idx LED 索引 (0-5)
 * @param r/g/b 颜色分量 (0-255)
 * @param percent 亮度百分比 (0-100)，本项目统一用 50
 */
void set_led_color_pct(uint8_t led_idx, uint8_t r, uint8_t g, uint8_t b, uint8_t percent) {
  if (led_idx >= LED_COUNT) return;
  if (percent > 100) percent = 100;

  uint16_t f = (uint16_t)percent * 255 / 100;  // 亮度比例 (0-255)

  led_grb[led_idx][0] = (uint8_t)((uint16_t)g * f / 255);  // G
  led_grb[led_idx][1] = (uint8_t)((uint16_t)r * f / 255);  // R
  led_grb[led_idx][2] = (uint8_t)((uint16_t)b * f / 255);  // B

  ws2812_dirty = true;  // 标记需要刷新
}

/**
 * @brief 熄灭所有 LED
 */
void Lights_TurnOffAll(void) {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    led_grb[i][0] = led_grb[i][1] = led_grb[i][2] = 0;
  }
  ws2812_dirty = true;
  Lights_Refresh();
}

/**
 * @brief 灯光系统初始化（全灭）
 */
void Lights_Init(void) {
  /* 默认全灭 */
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    led_grb[i][0] = led_grb[i][1] = led_grb[i][2] = 0;
  }
  ws2812_dirty = true;
  /* 先发一帧全灭，将灯珠锁存到关闭状态 */
  Lights_Refresh();
}

/**
 * @brief 灯光任务处理（需在主循环中周期性调用）
 * @note 颜色由应用层通过 set_led_color_pct 设置，此处仅负责发送
 */
void Lights_Task(void) {
  Lights_Refresh();
}
