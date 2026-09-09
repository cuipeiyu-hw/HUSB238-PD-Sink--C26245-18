#ifndef LIGHTS_H
#define LIGHTS_H

#include "main.h"
#include "spi.h"

/* 灯珠数量（6 颗 WS2812，串联在同一 SPI 数据链上） */
#define LED_COUNT 6

/*
 * WS2812 驱动方案：SPI1(PA7/MOSI) + DMA（DMA1_Channel1）
 *  - 每个 WS2812 位用 4 个 SPI 位表示："0" = 0b1000, "1" = 0b1110
 *  - SPI 时钟 = PCLK1 / 16 = 64MHz / 16 = 4MHz（每 SPI 位 0.25us）
 *      一个 WS2812 位 = 4 SPI 位 = 1.0us
 *      "0"：T0H=0.25us / T0L=0.75us；"1"：T1H=0.75us / T1L=0.25us，均在规格内
 *      （此前 3 位编码@2MHz 的 T0H=0.5us 卡在判 0/1 阈值边缘，导致"0"被误判为"1"→全白）
 *  - 每颗灯 24bit(GRB) => 96 SPI 位 => 12 字节
 */
#define WS2812_SPI_BYTES_PER_LED 12

/* 复位/锁存需要的低电平时间 >50us；4MHz 下每字节 8bit*0.25us = 2us，30 字节 ≈ 60us */
#define WS2812_RESET_BYTES 30

/* SPI 发送缓冲（数据区 + 复位区） */
#define WS2812_BUF_SIZE (LED_COUNT * WS2812_SPI_BYTES_PER_LED + WS2812_RESET_BYTES)

/* RGB 颜色（注意 WS2812 线上顺序为 G-R-B） */
typedef struct {
    uint8_t g;
    uint8_t r;
    uint8_t b;
} LED_Color_t;

/* 函数声明 */
void Lights_Init(void);
void Lights_TurnOffAll(void);
void Lights_Task(void);

/* 直接设置某灯 RGB（0-255） */
void set_led_color(uint8_t led_idx, uint8_t r, uint8_t g, uint8_t b);
/* 以指定亮度百分比设置某灯 RGB 颜色（percent 0-100，统一 50% 亮度用） */
void set_led_color_pct(uint8_t led_idx, uint8_t r, uint8_t g, uint8_t b, uint8_t percent);
/* 若状态有更新且 DMA 空闲，则通过 SPI+DMA 发送最新一帧 */
void Lights_Refresh(void);
/* 在 HAL_SPI_TxCpltCallback 中调用，通知发送完成并可补发最新状态 */
void Lights_DMA_TxCpltCallback(void);

#endif // LIGHTS_H
