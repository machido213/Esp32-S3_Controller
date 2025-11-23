#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h" // 確保包含 ADC 定義

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================
// ESP32-S3-DevKitC-1 專用配置
// 避開 Flash(26-32), USB(19-20), Console(43-44), 空洞(22-25)
// =============================================================

// OTA 更新按鈕（Z1）- 使用 GPIO 0 (板子上的 BOOT 按鈕，方便測試)
// 如果要外接按鈕，可改用 GPIO 3 或 21
#ifndef Z1_GPIO
#define Z1_GPIO 0 
#endif

// ---------- 電源端 (A 系列 - 放在左側上部) ----------
#ifndef A1_1_GPIO
#define A1_1_GPIO 4
#endif
#ifndef A1_2_GPIO
#define A1_2_GPIO 5
#endif

// 狀態指示燈 (輸出)
#ifndef A2_GPIO
#define A2_GPIO 6
#endif
#ifndef A3_GPIO
#define A3_GPIO 7
#endif
#ifndef A4_GPIO
#define A4_GPIO 15 // 跳過 8~14 以分散布局，15 在左側
#endif

// ---------- 選擇端 (B 系列) ----------
// 三檔位開關 B1
#ifndef B1_1_GPIO
#define B1_1_GPIO 16 // 左側
#endif
#ifndef B1_2_GPIO
#define B1_2_GPIO 17 // 左側
#endif

// ★★★ 關鍵修改：電位器 (ADC) ★★★
// 必須使用 ADC1 (GPIO 1~10) 以避免與 Wi-Fi 衝突
// 根據圖片右側，GPIO 1 和 2 是 ADC1 通道
#ifndef B2_GPIO
#define B2_GPIO 1 // 右側 (ADC1_CH0)
#endif
#ifndef B3_GPIO
#define B3_GPIO 2 // 右側 (ADC1_CH1)
#endif

// ADC 通道映射 (配合上面的 GPIO 1, 2)
#ifndef B2_ADC_CHANNEL
#define B2_ADC_CHANNEL ADC_CHANNEL_0
#endif
#ifndef B3_ADC_CHANNEL
#define B3_ADC_CHANNEL ADC_CHANNEL_1
#endif

// 開關 B4, B5
#ifndef B4_GPIO
#define B4_GPIO 8 // 左側
#endif
#ifndef B5_GPIO
#define B5_GPIO 9 // 左側
#endif

// 蜂鳴器 B6 (輸出)
#ifndef B6_GPIO
#define B6_GPIO 10 // 左側
#endif

// ---------- 搖桿端 (C 系列 - 使用剩餘的安全腳位) ----------
// 我們集中使用左側剩餘的 ADC2 腳位(當數位用沒問題) 和右側的 35-42

// C1: 雙軸 (使用左側下半部)
#ifndef C1_1_GPIO
#define C1_1_GPIO 11
#endif
#ifndef C1_2_GPIO
#define C1_2_GPIO 12
#endif
#ifndef C1_3_GPIO
#define C1_3_GPIO 13
#endif
#ifndef C1_4_GPIO
#define C1_4_GPIO 14
#endif

// C2: 雙軸 (使用右側中段)
#ifndef C2_1_GPIO
#define C2_1_GPIO 38
#endif
#ifndef C2_2_GPIO
#define C2_2_GPIO 39
#endif
#ifndef C2_3_GPIO
#define C2_3_GPIO 40
#endif
#ifndef C2_4_GPIO
#define C2_4_GPIO 41
#endif

// C3: 雙軸 (使用右側中段 & 特殊腳)
#ifndef C3_1_GPIO
#define C3_1_GPIO 42
#endif
#ifndef C3_2_GPIO
#define C3_2_GPIO 21 // 右下角
#endif
#ifndef C3_3_GPIO
#define C3_3_GPIO 35 // 右側 (注意：若板子是 Octal PSRAM 此腳不可用，一般 WROOM-1 可用)
#endif
#ifndef C3_4_GPIO
#define C3_4_GPIO 36 // 右側
#endif

// C4: 單軸 (2 腳)
#ifndef C4_1_GPIO
#define C4_1_GPIO 37 // 右側
#endif
#ifndef C4_2_GPIO
#define C4_2_GPIO 45 // 右下 (Strap pin，開機時請勿拉低)
#endif

// ---------- UART 通訊 (Jetson) ----------
// 移至右下角 47, 48，這兩個腳位完全獨立且安全
#ifndef JETSON_UART_NUM
#define JETSON_UART_NUM UART_NUM_1
#endif
#ifndef JETSON_UART_TX_PIN
#define JETSON_UART_TX_PIN 46
#endif
#ifndef JETSON_UART_RX_PIN
#define JETSON_UART_RX_PIN 47
#endif
#ifndef JETSON_UART_BAUD
#define JETSON_UART_BAUD 115200
#endif

// =============================================================
void io_init(void);
int io_read_pin(gpio_num_t pin);
void io_write_pin(gpio_num_t pin, int level);

#ifdef __cplusplus
}
#endif