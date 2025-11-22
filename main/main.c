/*
 * Integrated example: IO init, UART comms, HTTP server (status + OTA trigger),
 * and OTA task. All in a single file for simpler projects.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/uart.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "io_config.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "esp32_s3_ctrl";

/* ---------- IO helpers (from io_config.h) ---------- */
void io_init(void)
{
    // 初始化所有輸入與輸出腳位（以 README 中的分類為準）
    // 1) 設定所有數位輸入腳位 (上拉以避免浮動)
    uint64_t in_mask = 0ULL;
    in_mask |= (1ULL<<Z1_GPIO);
    in_mask |= (1ULL<<A1_1_GPIO);
    in_mask |= (1ULL<<A1_2_GPIO);
    in_mask |= (1ULL<<B1_1_GPIO);
    in_mask |= (1ULL<<B1_2_GPIO);
    // B2/B3 為電位器(ADC)，不要在此設為上拉，會改用獨立設定
    in_mask |= (1ULL<<B4_GPIO);
    in_mask |= (1ULL<<B5_GPIO);
    in_mask |= (1ULL<<C1_1_GPIO);
    in_mask |= (1ULL<<C1_2_GPIO);
    in_mask |= (1ULL<<C1_3_GPIO);
    in_mask |= (1ULL<<C1_4_GPIO);
    in_mask |= (1ULL<<C2_1_GPIO);
    in_mask |= (1ULL<<C2_2_GPIO);
    in_mask |= (1ULL<<C2_3_GPIO);
    in_mask |= (1ULL<<C2_4_GPIO);
    in_mask |= (1ULL<<C3_1_GPIO);
    in_mask |= (1ULL<<C3_2_GPIO);
    in_mask |= (1ULL<<C3_3_GPIO);
    in_mask |= (1ULL<<C3_4_GPIO);
    in_mask |= (1ULL<<C4_1_GPIO);
    in_mask |= (1ULL<<C4_2_GPIO);

    gpio_config_t in_conf = {
        .pin_bit_mask = in_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&in_conf);

    // 2) 對於 ADC 腳 (B2/B3)，單獨設定為輸入且不啟用上拉/下拉
    uint64_t adc_mask = 0ULL;
    adc_mask |= (1ULL<<B2_GPIO);
    adc_mask |= (1ULL<<B3_GPIO);
    gpio_config_t adc_conf = {
        .pin_bit_mask = adc_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&adc_conf);

    // 3) 設定輸出腳位 (指示燈、蜂鳴器)
    uint64_t out_mask = 0ULL;
    out_mask |= (1ULL<<A2_GPIO);
    out_mask |= (1ULL<<A3_GPIO);
    out_mask |= (1ULL<<A4_GPIO);
    out_mask |= (1ULL<<B6_GPIO);

    gpio_config_t out_conf = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&out_conf);

    ESP_LOGI(TAG, "IO initialized (Z1,A1,B1,B2,B3,B4,B5,C1-4) and outputs A2-A4,B6");
}

int io_read_pin(gpio_num_t pin)
{
    return gpio_get_level(pin);
}

void io_write_pin(gpio_num_t pin, int level)
{
    gpio_set_level(pin, level);
}

/* ---------- UART comms (simple) ---------- */
static const char *UART_TAG = "comms_uart";

void comms_uart_init(void)
{
    const uart_port_t uart_num = JETSON_UART_NUM;
    uart_config_t uart_config = {
        .baud_rate = JETSON_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(uart_num, &uart_config);
    uart_set_pin(uart_num, JETSON_UART_TX_PIN, JETSON_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uart_num, 2048, 0, 0, NULL, 0);
    ESP_LOGI(UART_TAG, "UART initialized: num=%d tx=%d rx=%d baud=%d",
             uart_num, JETSON_UART_TX_PIN, JETSON_UART_RX_PIN, JETSON_UART_BAUD);
}

void comms_uart_send_status(const char *json)
{
    if (!json) return;
    const uart_port_t uart_num = JETSON_UART_NUM;
    size_t len = strlen(json);
    uart_write_bytes(uart_num, json, len);
    uart_write_bytes(uart_num, "\n", 1);
}

// 讀取電位器的 placeholder（實務上建議使用 ADC1/ADC2 擷取，這裡回傳 0~4095 範圍的模擬值）
// TODO: 根據實際板子將 B2_GPIO/B3_GPIO 映射到 ADC 通道，並使用 adc1_get_raw
static int read_pot_raw(int which)
{
    // 使用 esp_adc 的 oneshot API
    // 這裡在第一次呼叫時建立 ADC unit handle 並設定 channel
    static bool adc_inited = false;
    static adc_oneshot_unit_handle_t adc_handle = NULL;
    if (!adc_inited) {
        adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &adc_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
            return 0;
        }

        adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_12,
            .atten = ADC_ATTEN_DB_12,
        };
        adc_oneshot_config_channel(adc_handle, B2_ADC_CHANNEL, &chan_cfg);
        adc_oneshot_config_channel(adc_handle, B3_ADC_CHANNEL, &chan_cfg);

        adc_inited = true;
    }

    int raw = 0;
    esp_err_t r;
    if (which == 2) {
        r = adc_oneshot_read(adc_handle, B2_ADC_CHANNEL, &raw);
    } else if (which == 3) {
        r = adc_oneshot_read(adc_handle, B3_ADC_CHANNEL, &raw);
    } else {
        return 0;
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "adc read failed: %s", esp_err_to_name(r));
        return 0;
    }
    return raw; // 0..4095 (depending on bitwidth)
}

/* ---------- OTA (background using esp_https_ota) ---------- */
static const char *OTA_TAG = "ota_task";

static void ota_task(void *arg)
{
    char *url = (char *)arg; // malloc'ed by caller
    if (!url) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(OTA_TAG, "OTA task start: %s", url);

    // 使用 esp_https_ota_config_t 並把 http client config 放入 http_config
    // esp_https_ota expects a pointer to esp_http_client_config_t inside esp_https_ota_config_t
    esp_http_client_config_t http_cfg = {
        .url = url,
        // .cert_pem = (char *)server_cert_pem_start, // 正式環境請提供憑證
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_cfg,
    };
    esp_err_t err = esp_https_ota(&ota_config);
    if (err == ESP_OK) {
        ESP_LOGI(OTA_TAG, "OTA successful, rebooting...");
        free(url);
        esp_restart();
    } else {
        ESP_LOGE(OTA_TAG, "OTA failed: %s", esp_err_to_name(err));
        free(url);
    }
    vTaskDelete(NULL);
}

void ota_start(const char *url)
{
    if (!url) return;
    char *p = strdup(url);
    if (!p) return;
    xTaskCreate(ota_task, "ota_task", 8192, p, 5, NULL);
}

/* ---------- HTTP server (status + ota trigger) ---------- */
static const char *WS_TAG = "web_server";
static httpd_handle_t server = NULL;

static esp_err_t status_get_handler(httpd_req_t *req)
{
    // 回傳完整狀態 JSON (包含電源端、選擇端、搖桿端、電位器等)
    char buf[512];

    int a1_1 = io_read_pin(A1_1_GPIO);
    int a1_2 = io_read_pin(A1_2_GPIO);
    int a2 = io_read_pin(A2_GPIO);
    int a3 = io_read_pin(A3_GPIO);
    int a4 = io_read_pin(A4_GPIO);

    int b1_1 = io_read_pin(B1_1_GPIO);
    int b1_2 = io_read_pin(B1_2_GPIO);
    int b4 = io_read_pin(B4_GPIO);
    int b5 = io_read_pin(B5_GPIO);

    int c1_1 = io_read_pin(C1_1_GPIO);
    int c1_2 = io_read_pin(C1_2_GPIO);
    int c1_3 = io_read_pin(C1_3_GPIO);
    int c1_4 = io_read_pin(C1_4_GPIO);

    int c2_1 = io_read_pin(C2_1_GPIO);
    int c2_2 = io_read_pin(C2_2_GPIO);
    int c2_3 = io_read_pin(C2_3_GPIO);
    int c2_4 = io_read_pin(C2_4_GPIO);

    int c3_1 = io_read_pin(C3_1_GPIO);
    int c3_2 = io_read_pin(C3_2_GPIO);
    int c3_3 = io_read_pin(C3_3_GPIO);
    int c3_4 = io_read_pin(C3_4_GPIO);

    int c4_1 = io_read_pin(C4_1_GPIO);
    int c4_2 = io_read_pin(C4_2_GPIO);

    int pot_b2 = read_pot_raw(2);
    int pot_b3 = read_pot_raw(3);

    int len = snprintf(buf, sizeof(buf),
                       "{\"A1_1\":%d,\"A1_2\":%d,\"A2\":%d,\"A3\":%d,\"A4\":%d,"
                       "\"B1_1\":%d,\"B1_2\":%d,\"B4\":%d,\"B5\":%d,\"B2_pot\":%d,\"B3_pot\":%d,"
                       "\"C1\":[%d,%d,%d,%d],\"C2\":[%d,%d,%d,%d],\"C3\":[%d,%d,%d,%d],\"C4\":[%d,%d] }\n",
                       a1_1, a1_2, a2, a3, a4,
                       b1_1, b1_2, b4, b5, pot_b2, pot_b3,
                       c1_1, c1_2, c1_3, c1_4,
                       c2_1, c2_2, c2_3, c2_4,
                       c3_1, c3_2, c3_3, c3_4,
                       c4_1, c4_2);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);

    // 同步透過 UART 傳給 Jetson
    comms_uart_send_status(buf);

    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    char url[256] = {0};
    int total_len = req->content_len;
    if (total_len > 0 && total_len < (int)sizeof(url)) {
        int ret = httpd_req_recv(req, url, total_len);
        if (ret > 0) url[ret < (int)sizeof(url) ? ret : (int)sizeof(url)-1] = '\0';
    }

    // If body looks like JSON, try to extract "url":"..."
    if (url[0] == '{') {
        char *p = strstr(url, "\"url\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '"') p++;
                char *end = p;
                while (*end && *end != '"' && *end != '}') end++;
                size_t n = (end - p);
                if (n >= sizeof(url)) n = sizeof(url)-1;
                memmove(url, p, n);
                url[n] = 0;
            }
        }
    }

    // fallback: check query string
    if (strlen(url) == 0) {
        char qs[256];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
            char val[256];
            if (httpd_query_key_value(qs, "url", val, sizeof(val)) == ESP_OK) {
                strncpy(url, val, sizeof(url)-1);
            }
        }
    }

    if (strlen(url) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_FAIL;
    }

    ESP_LOGI(WS_TAG, "OTA requested: %s", url);
    ota_start(url);

    httpd_resp_sendstr(req, "OTA started\n");
    return ESP_OK;
}

static const httpd_uri_t status_uri = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t ota_uri = {
    .uri       = "/ota",
    .method    = HTTP_POST,
    .handler   = ota_post_handler,
    .user_ctx  = NULL
};

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(WS_TAG, "Failed to start server");
        return ESP_FAIL;
    }
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &ota_uri);
    ESP_LOGI(WS_TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    return ESP_OK;
}

/* ---------- Status task + app_main ---------- */
static void status_task(void *arg)
{
    char json[256];
    // 儲存選擇端的三個目標變數 (以 B1 做選擇，B3 做輸入時儲存)
    static int stored_vals[3] = {0,0,0}; // 0:試體/目標編號, 1:橫軸, 2:縱軸/高度  (示意)

    while (1) {
        // 讀取電源端三檔位 (A1_1, A1_2) 並決定哪個指示燈要亮
        int a1_1 = io_read_pin(A1_1_GPIO);
        int a1_2 = io_read_pin(A1_2_GPIO);

        // 根據 README 規則：若 A1_1 與 A1_2 同時為高，則 A2 高
        // 否則 A1_1->A3, A1_2->A4
        if (a1_1 && a1_2) {
            // 兩者同時為高 -> 顯示 A2
            io_write_pin(A2_GPIO, 1);
            io_write_pin(A3_GPIO, 0);
            io_write_pin(A4_GPIO, 0);
        } else if (a1_1) {
            io_write_pin(A2_GPIO, 0);
            io_write_pin(A3_GPIO, 1);
            io_write_pin(A4_GPIO, 0);
        } else if (a1_2) {
            io_write_pin(A2_GPIO, 0);
            io_write_pin(A3_GPIO, 0);
            io_write_pin(A4_GPIO, 1);
        } else {
            // 預設：全部熄滅
            io_write_pin(A2_GPIO, 0);
            io_write_pin(A3_GPIO, 0);
            io_write_pin(A4_GPIO, 0);
        }

        // 如果 A3 (電源端某狀態) 啟用時才讀取選擇端
        if (io_read_pin(A3_GPIO)) {
            int b4 = io_read_pin(B4_GPIO);
            int b5 = io_read_pin(B5_GPIO);

            // 讀取電位器值（此處為 placeholder）
            int val = b4 ? read_pot_raw(3) : read_pot_raw(2);

            // B1 選擇要存放到哪個變數 (用 B1_1/B1_2 的組合來決定)
            int b1_1 = io_read_pin(B1_1_GPIO);
            int b1_2 = io_read_pin(B1_2_GPIO);
            int idx = 0;
            // 簡單 mapping：00 -> 0 (試體), 01 -> 1 (橫軸), 10 -> 2 (高度)
            if (b1_1 == 0 && b1_2 == 0) idx = 0;
            else if (b1_1 == 0 && b1_2 == 1) idx = 1;
            else if (b1_1 == 1 && b1_2 == 0) idx = 2;
            else idx = 0;

            // 如果點動開關按下 (B5) 則儲存目前選取的值並送出給 Jetson
            if (b5) {
                stored_vals[idx] = val;
                // 送出儲存後的值給 Jetson
                int l = snprintf(json, sizeof(json), "{\"stored_idx\":%d,\"val\":%d}", idx, val);
                if (l>0) comms_uart_send_status(json);
                // 觸發蜂鳴器短促提示
                io_write_pin(B6_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                io_write_pin(B6_GPIO, 0);
            }
        }

        // 搖桿讀取與組 JSON（簡化：讀取方向開關狀態）
        int c1_1 = io_read_pin(C1_1_GPIO);
        int c1_2 = io_read_pin(C1_2_GPIO);
        int c1_3 = io_read_pin(C1_3_GPIO);
        int c1_4 = io_read_pin(C1_4_GPIO);

        int c2_1 = io_read_pin(C2_1_GPIO);
        int c2_2 = io_read_pin(C2_2_GPIO);
        int c2_3 = io_read_pin(C2_3_GPIO);
        int c2_4 = io_read_pin(C2_4_GPIO);

        int c3_1 = io_read_pin(C3_1_GPIO);
        int c3_2 = io_read_pin(C3_2_GPIO);
        int c3_3 = io_read_pin(C3_3_GPIO);
        int c3_4 = io_read_pin(C3_4_GPIO);

        int c4_1 = io_read_pin(C4_1_GPIO);
        int c4_2 = io_read_pin(C4_2_GPIO);

        int len = snprintf(json, sizeof(json),
                           "{\"A\":[%d,%d],\"LED\":[%d,%d,%d],\"B_stored\":[%d,%d,%d],\"C1\":[%d,%d,%d,%d],\"C2\":[%d,%d,%d,%d],\"C3\":[%d,%d,%d,%d],\"C4\":[%d,%d]}",
                           a1_1, a1_2,
                           io_read_pin(A2_GPIO), io_read_pin(A3_GPIO), io_read_pin(A4_GPIO),
                           stored_vals[0], stored_vals[1], stored_vals[2],
                           c1_1,c1_2,c1_3,c1_4,
                           c2_1,c2_2,c2_3,c2_4,
                           c3_1,c3_2,c3_3,c3_4,
                           c4_1,c4_2);
        if (len>0) comms_uart_send_status(json);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "Starting esp32_s3_controller (integrated)");

    /* 初始化 NVS 與網路/事件迴圈，確保 lwIP/tcpip 已就緒再啟動 HTTP 伺服器 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    io_init();
    comms_uart_init();

    /* 現在安全啟動 HTTP 伺服器 */
    if (web_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "web_server_start failed");
    }

    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
}