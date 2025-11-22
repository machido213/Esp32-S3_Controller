#pragma once

#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================
// IO 腳位預設（請根據實際接線修改數值）
// 本檔案提供所有元件的 GPIO 定義（包含電源端/選擇端/搖桿端/OTA）
// 若要改用 menuconfig 管理，請改為用 sdkconfig 設定。
// =============================================================

// OTA 更新按鈕（Z1）
#ifndef Z1_GPIO
#define Z1_GPIO 2  // OTA 按鈕（輸入）
#endif

// ---------- 電源端 (A 系列) ----------
// 三檔位開關 A1 需要兩個輸入（A1_1, A1_2）以表示三種狀態
#ifndef A1_1_GPIO
#define A1_1_GPIO 4
#endif
#ifndef A1_2_GPIO
#define A1_2_GPIO 5
#endif

// 狀態指示燈 A2~A4（輸出）
#ifndef A2_GPIO
#define A2_GPIO 6
#endif
#ifndef A3_GPIO
#define A3_GPIO 7
#endif
#ifndef A4_GPIO
#define A4_GPIO 8
#endif

// ---------- 選擇端 (B 系列) ----------
// 三檔位開關 B1（2 個輸入，決定存放目標變數）
#ifndef B1_1_GPIO
#define B1_1_GPIO 9
#endif
#ifndef B1_2_GPIO
#define B1_2_GPIO 10
#endif

// 電位器 B2/B3（通常為模擬，這裡預設為數位/佔位腳位；如要使用 ADC 請改為對應 ADC 通道）
#ifndef B2_GPIO
#define B2_GPIO 11 // 電位器/模擬輸入 (占位)
#endif
#ifndef B3_GPIO
#define B3_GPIO 12 // 電位器/模擬輸入 (占位)
#endif

// 若要把 B2/B3 對應到 ADC channel，請在此設定對應的 ADC channel
// 注意：選擇的 ADC channel 必須與實際接腳相符。以下為範例映射（可修改）：
#ifndef B2_ADC_CHANNEL
#define B2_ADC_CHANNEL ADC_CHANNEL_0
#endif
#ifndef B3_ADC_CHANNEL
#define B3_ADC_CHANNEL ADC_CHANNEL_1
#endif

// 切換開關 B4, 點動開關 B5（數位輸入）
#ifndef B4_GPIO
#define B4_GPIO 13
#endif
#ifndef B5_GPIO
#define B5_GPIO 14
#endif

// 蜂鳴器 B6（輸出，若為 220V 請接繼電器驅動）
#ifndef B6_GPIO
#define B6_GPIO 15
#endif

// ---------- 搖桿端 (C 系列) ----------
// 每個雙軸搖桿使用 4 個腳位 (X+/X-/Y+/Y-)，單軸 2 個
// C1: 雙軸
#ifndef C1_1_GPIO
#define C1_1_GPIO 16
#endif
#ifndef C1_2_GPIO
#define C1_2_GPIO 17
#endif
#ifndef C1_3_GPIO
#define C1_3_GPIO 18
#endif
#ifndef C1_4_GPIO
#define C1_4_GPIO 19
#endif

// C2: 雙軸
#ifndef C2_1_GPIO
#define C2_1_GPIO 20
#endif
#ifndef C2_2_GPIO
#define C2_2_GPIO 21
#endif
#ifndef C2_3_GPIO
#define C2_3_GPIO 22
#endif
#ifndef C2_4_GPIO
#define C2_4_GPIO 23
#endif

// C3: 雙軸
#ifndef C3_1_GPIO
#define C3_1_GPIO 25
#endif
#ifndef C3_2_GPIO
#define C3_2_GPIO 26
#endif
#ifndef C3_3_GPIO
#define C3_3_GPIO 27
#endif
#ifndef C3_4_GPIO
#define C3_4_GPIO 28
#endif

// C4: 單軸 (2 個腳位)
#ifndef C4_1_GPIO
#define C4_1_GPIO 29
#endif
#ifndef C4_2_GPIO
#define C4_2_GPIO 30
#endif

// ---------- UART 與其他通訊定義 ----------
#ifndef JETSON_UART_NUM
#define JETSON_UART_NUM UART_NUM_1
#endif
#ifndef JETSON_UART_TX_PIN
#define JETSON_UART_TX_PIN 33
#endif
#ifndef JETSON_UART_RX_PIN
#define JETSON_UART_RX_PIN 34
#endif
#ifndef JETSON_UART_BAUD
#define JETSON_UART_BAUD 115200
#endif

// =============================================================
// 初始化 IO（會設定 input / pull / output）
void io_init(void);

// 讀取/寫入函式樣板（可擴充）
int io_read_pin(gpio_num_t pin);
void io_write_pin(gpio_num_t pin, int level);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
}
#endif