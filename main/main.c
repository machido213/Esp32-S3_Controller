/*
 * ESP32-S3 Controller with WiFi Provisioning & NVS
 * 功能總覽：
 * 1. NVS: 斷電記憶 WiFi 帳密與固定 IP。
 * 2. WiFi: 優先連線，失敗自動切換為 AP 熱點模式 (救援模式)。
 * 3. SPIFFS: 存放 index.html 網頁檔。
 * 4. Web Server: 提供網頁監控、OTA 更新、WiFi 設定修改。
 * 5. IO/UART: 讀取搖桿/開關狀態，透過 UART 傳送 JSON 給 Jetson Orin Nano。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h> // 提供 MIN() 巨集，解決編譯錯誤
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/uart.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "io_config.h" // 包含所有 GPIO 腳位定義
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include "cJSON.h"     // 用於解析與產生 JSON 資料
#include "esp_crt_bundle.h" // 用於 HTTPS OTA 的憑證驗證

// --- Log 標籤 ---
static const char *TAG = "CONTROLLER";

// --- WiFi 預設出廠值 (當 NVS 無資料時使用) ---
#define DEFAULT_SSID      "SSID"
#define DEFAULT_PASS      "********"
#define DEFAULT_IP        "192.168.2.123"
#define DEFAULT_GW        "192.168.2.1"
#define DEFAULT_MASK      "255.255.255.0"

// --- AP 救援模式設定 (當連線失敗時啟動的熱點) ---
#define AP_SSID           "ESP32-Controller-Rescue"
#define AP_PASS           "" // 空字串代表無密碼，方便緊急連線
#define MAX_RETRY         5  // 最大重試連線次數

// --- WiFi 事件群組與旗標 ---
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0 // 連線成功旗標
#define WIFI_FAIL_BIT      BIT1 // 連線失敗旗標
static int s_retry_num = 0;     // 目前重試次數計數器

// --- 系統設定結構體 (用於暫存 NVS 讀出的資料) ---
typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char static_ip[16];
    char static_gw[16];
    char static_mask[16];
} SystemConfig;

SystemConfig sys_cfg;

/* ==========================================================
 * 1. NVS 讀寫功能 (資料儲存)
 * ========================================================== */

// 從 Flash 讀取設定 (開機時呼叫)
void load_settings() {
    nvs_handle_t my_handle;
    // 打開 NVS 的 "storage" 命名空間
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, loading defaults.");
        // 若打開失敗，載入預設值
        strcpy(sys_cfg.wifi_ssid, DEFAULT_SSID);
        strcpy(sys_cfg.wifi_pass, DEFAULT_PASS);
        strcpy(sys_cfg.static_ip, DEFAULT_IP);
        strcpy(sys_cfg.static_gw, DEFAULT_GW);
        strcpy(sys_cfg.static_mask, DEFAULT_MASK);
        return;
    }
    // 依序讀取各個欄位，若讀不到則使用預設值
    size_t size = sizeof(sys_cfg.wifi_ssid);
    if (nvs_get_str(my_handle, "ssid", sys_cfg.wifi_ssid, &size) != ESP_OK) strcpy(sys_cfg.wifi_ssid, DEFAULT_SSID);
    
    size = sizeof(sys_cfg.wifi_pass);
    if (nvs_get_str(my_handle, "pass", sys_cfg.wifi_pass, &size) != ESP_OK) strcpy(sys_cfg.wifi_pass, DEFAULT_PASS);
    
    size = sizeof(sys_cfg.static_ip);
    if (nvs_get_str(my_handle, "ip", sys_cfg.static_ip, &size) != ESP_OK) strcpy(sys_cfg.static_ip, DEFAULT_IP);
    
    size = sizeof(sys_cfg.static_gw);
    if (nvs_get_str(my_handle, "gw", sys_cfg.static_gw, &size) != ESP_OK) strcpy(sys_cfg.static_gw, DEFAULT_GW);
    
    size = sizeof(sys_cfg.static_mask);
    if (nvs_get_str(my_handle, "mask", sys_cfg.static_mask, &size) != ESP_OK) strcpy(sys_cfg.static_mask, DEFAULT_MASK);
    
    nvs_close(my_handle);
}

// 寫入設定到 Flash (網頁修改時呼叫)
void save_settings(const char* ssid, const char* pass, const char* ip, const char* gw, const char* mask) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "ssid", ssid);
        nvs_set_str(my_handle, "pass", pass);
        nvs_set_str(my_handle, "ip", ip);
        nvs_set_str(my_handle, "gw", gw);
        nvs_set_str(my_handle, "mask", mask);
        nvs_commit(my_handle); // 務必 commit 才會真正寫入
        nvs_close(my_handle);
    }
}

/* ==========================================================
 * 2. WiFi 事件處理與初始化 (連線邏輯)
 * ========================================================== */

// WiFi 事件監聽器 (處理連線成功、失敗、斷線重連)
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); // WiFi 啟動後，立刻嘗試連線
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 若斷線或連線失敗，檢查是否超過重試次數
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retry to connect to the AP (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            // 超過次數，設定「失敗旗標」，準備切換到 AP 模式
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 成功取得 IP
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// 初始化 WiFi Station 模式 (連線路由器)
// 回傳 true: 連線成功, false: 連線失敗
bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_t *my_sta = esp_netif_create_default_wifi_sta();

    // --- 設定固定 IP (Static IP) ---
    esp_netif_dhcpc_stop(my_sta); // 關閉 DHCP
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton(sys_cfg.static_ip);
    ip_info.gw.addr = esp_ip4addr_aton(sys_cfg.static_gw);
    ip_info.netmask.addr = esp_ip4addr_aton(sys_cfg.static_mask);
    esp_netif_set_ip_info(my_sta, &ip_info);

    // 設定 DNS (使用 Google DNS 8.8.8.8 以確保 OTA 可用)
    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
    dns_info.ip.type = IPADDR_TYPE_V4;
    esp_netif_set_dns_info(my_sta, ESP_NETIF_DNS_MAIN, &dns_info);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 註冊事件監聽
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    // 設定 WiFi 帳密
    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, sys_cfg.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, sys_cfg.wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", sys_cfg.wifi_ssid);

    // 等待連線結果 (阻塞直到成功或失敗)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) return true;
    return false;
}

// 初始化 WiFi AP 模式 (救援熱點)
void wifi_init_ap(void)
{
    esp_wifi_stop(); // 先停止目前的 STA 嘗試
    esp_wifi_set_mode(WIFI_MODE_NULL);

    esp_netif_create_default_wifi_ap();
    
    // 設定 AP 參數
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN // 開放網路，無密碼
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(TAG, "Switched to AP Mode: %s (IP: 192.168.4.1)", AP_SSID);
}

/* ==========================================================
 * 3. IO 與 硬體控制 (GPIO, UART, ADC)
 * ========================================================== */

// 初始化 GPIO
void io_init(void)
{
    // 設定所有輸入腳位 (上拉電阻，避免浮動)
    // 包含電源端(A1)、選擇端(B1, B4, B5)、搖桿端(C1~C4)、OTA按鈕(Z1)
    uint64_t in_mask = (1ULL<<Z1_GPIO) | (1ULL<<A1_1_GPIO) | (1ULL<<A1_2_GPIO) | (1ULL<<B4_GPIO) | (1ULL<<B5_GPIO);
    in_mask |= (1ULL<<C1_1_GPIO) | (1ULL<<C1_2_GPIO) | (1ULL<<C1_3_GPIO) | (1ULL<<C1_4_GPIO);
    in_mask |= (1ULL<<C2_1_GPIO) | (1ULL<<C2_2_GPIO) | (1ULL<<C2_3_GPIO) | (1ULL<<C2_4_GPIO);
    in_mask |= (1ULL<<C3_1_GPIO) | (1ULL<<C3_2_GPIO) | (1ULL<<C3_3_GPIO) | (1ULL<<C3_4_GPIO);
    in_mask |= (1ULL<<C4_1_GPIO) | (1ULL<<C4_2_GPIO);
    in_mask |= (1ULL<<B1_1_GPIO) | (1ULL<<B1_2_GPIO);

    gpio_config_t in_conf = { .pin_bit_mask = in_mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in_conf);

    // 設定 ADC 輸入腳 (B2, B3) - 類比輸入不能有 Pull-up
    uint64_t adc_mask = (1ULL<<B2_GPIO) | (1ULL<<B3_GPIO);
    gpio_config_t adc_conf = { .pin_bit_mask = adc_mask, .mode = GPIO_MODE_INPUT, .pull_up_en = 0 };
    gpio_config(&adc_conf);

    // 設定輸出腳位 (指示燈 A2~A4, 蜂鳴器 B6)
    uint64_t out_mask = (1ULL<<A2_GPIO) | (1ULL<<A3_GPIO) | (1ULL<<A4_GPIO) | (1ULL<<B6_GPIO);
    gpio_config_t out_conf = { .pin_bit_mask = out_mask, .mode = GPIO_MODE_INPUT_OUTPUT, .pull_up_en = 0 };
    gpio_config(&out_conf);
}

// 讀寫腳位 Helper 函式
int io_read_pin(gpio_num_t pin) { return gpio_get_level(pin); }
void io_write_pin(gpio_num_t pin, int level) { gpio_set_level(pin, level); }

// 初始化 UART (連接 Jetson Orin Nano)
void comms_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = JETSON_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(JETSON_UART_NUM, &uart_config);
    uart_set_pin(JETSON_UART_NUM, JETSON_UART_TX_PIN, JETSON_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(JETSON_UART_NUM, 2048, 0, 0, NULL, 0);
}

// 透過 UART 發送 JSON 字串
void comms_uart_send_status(const char *json) {
    if (!json) return;
    uart_write_bytes(JETSON_UART_NUM, json, strlen(json));
    uart_write_bytes(JETSON_UART_NUM, "\n", 1); // 補上換行符號
}

// 讀取電位器數值 (使用 ADC OneShot 模式)
static int read_pot_raw(int which) {
    static bool adc_inited = false;
    static adc_oneshot_unit_handle_t adc_handle = NULL;
    
    // 初始化 ADC 單元 (只執行一次)
    if (!adc_inited) {
        adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE };
        adc_oneshot_new_unit(&unit_cfg, &adc_handle);
        adc_oneshot_chan_cfg_t chan_cfg = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12 }; // 12-bit, 12dB (可讀到 3.3V)
        adc_oneshot_config_channel(adc_handle, B2_ADC_CHANNEL, &chan_cfg);
        adc_oneshot_config_channel(adc_handle, B3_ADC_CHANNEL, &chan_cfg);
        adc_inited = true;
    }
    
    int raw = 0;
    if (which == 2) adc_oneshot_read(adc_handle, B2_ADC_CHANNEL, &raw);
    else if (which == 3) adc_oneshot_read(adc_handle, B3_ADC_CHANNEL, &raw);
    return raw;
}

/* ==========================================================
 * 4. OTA 線上更新功能
 * ========================================================== */

// OTA 下載任務
static void ota_task(void *arg) {
    char *url = (char *)arg;
    ESP_LOGI(TAG, "Starting OTA: %s", url);
    
    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach, // 使用內建憑證包
        .keep_alive_enable = true,
        .timeout_ms = 10000,
        .buffer_size = 16384, // 加大緩衝區以應對大型 header
    };
    
    esp_https_ota_config_t ota_config = { .http_config = &http_cfg };
    
    if (esp_https_ota(&ota_config) == ESP_OK) {
        ESP_LOGI(TAG, "OTA Success, Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Failed");
    }
    free(url);
    vTaskDelete(NULL);
}

// 啟動 OTA 任務的進入點
void ota_start(const char *url) {
    if (!url) return;
    char *p = strdup(url);
    xTaskCreate(ota_task, "ota_task", 8192, p, 5, NULL);
}

/* ==========================================================
 * 5. Web Server (API 與 網頁)
 * ========================================================== */

// GET / : 讀取 index.html 並回傳
static esp_err_t root_get_handler(httpd_req_t *req) {
    FILE* f = fopen("/spiffs/index.html", "r");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); 
    return ESP_OK;
}

// GET /status : 回傳所有 IO 狀態的 JSON
static esp_err_t status_get_handler(httpd_req_t *req) {
    char buf[512];
    int b2 = read_pot_raw(2);
    int b3 = read_pot_raw(3);
    
    // 組裝 JSON 字串
    // 注意：io_read_pin 的回傳值 0/1 代表 Low/High
    snprintf(buf, sizeof(buf), 
        "{\"A1_1\":%d,\"A1_2\":%d,\"A2\":%d,\"A3\":%d,\"A4\":%d,"
        "\"B1_1\":%d,\"B1_2\":%d,\"B4\":%d,\"B5\":%d,\"B2_pot\":%d,\"B3_pot\":%d,"
        "\"C1\":[%d,%d,%d,%d],\"C2\":[%d,%d,%d,%d],\"C3\":[%d,%d,%d,%d],\"C4\":[%d,%d]}",
        io_read_pin(A1_1_GPIO), io_read_pin(A1_2_GPIO), io_read_pin(A2_GPIO), io_read_pin(A3_GPIO), io_read_pin(A4_GPIO),
        io_read_pin(B1_1_GPIO), io_read_pin(B1_2_GPIO), io_read_pin(B4_GPIO), io_read_pin(B5_GPIO), b2, b3,
        io_read_pin(C1_1_GPIO),io_read_pin(C1_2_GPIO),io_read_pin(C1_3_GPIO),io_read_pin(C1_4_GPIO),
        io_read_pin(C2_1_GPIO),io_read_pin(C2_2_GPIO),io_read_pin(C2_3_GPIO),io_read_pin(C2_4_GPIO),
        io_read_pin(C3_1_GPIO),io_read_pin(C3_2_GPIO),io_read_pin(C3_3_GPIO),io_read_pin(C3_4_GPIO),
        io_read_pin(C4_1_GPIO),io_read_pin(C4_2_GPIO)
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    
    // 順便把這份 JSON 透過 UART 傳給 Jetson (同步狀態)
    comms_uart_send_status(buf);
    return ESP_OK;
}

// POST /ota : 接收網頁傳來的 URL 並觸發更新
static esp_err_t ota_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf)-1));
    if(ret <= 0) return ESP_FAIL;
    buf[ret] = 0;
    
    cJSON *root = cJSON_Parse(buf);
    if(root) {
        cJSON *url = cJSON_GetObjectItem(root, "url");
        if(cJSON_IsString(url)) ota_start(url->valuestring);
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "OTA Starting...");
    } else {
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

// POST /api/save_wifi : 儲存新的 WiFi 設定並重啟
static esp_err_t api_save_wifi_handler(httpd_req_t *req) {
    char buf[512];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf)-1));
    if(ret <= 0) return ESP_FAIL;
    buf[ret] = 0;

    cJSON *root = cJSON_Parse(buf);
    if(!root) return ESP_FAIL;

    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    cJSON *j_ip = cJSON_GetObjectItem(root, "ip");
    cJSON *j_gw = cJSON_GetObjectItem(root, "gw");

    if(cJSON_IsString(j_ssid) && cJSON_IsString(j_pass) && cJSON_IsString(j_ip)) {
        save_settings(
            j_ssid->valuestring,
            j_pass->valuestring,
            j_ip->valuestring,
            (cJSON_IsString(j_gw)) ? j_gw->valuestring : DEFAULT_GW,
            "255.255.255.0"
        );
        httpd_resp_send(req, "Saved. Rebooting...", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        httpd_resp_send_500(req);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// 啟動 Web Server
static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        // 註冊 URI 路徑
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_uri_t status = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler };
        httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler };
        httpd_uri_t wifi = { .uri = "/api/save_wifi", .method = HTTP_POST, .handler = api_save_wifi_handler };
        
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &status);
        httpd_register_uri_handler(server, &ota);
        httpd_register_uri_handler(server, &wifi);
        ESP_LOGI(TAG, "Web Server Started");
    }
}

/* ==========================================================
 * 6. 主程式 (Main) 與 狀態迴圈
 * ========================================================== */

// 狀態更新任務 (處理燈號邏輯與蜂鳴器)
static void status_task(void *arg) {
    static int stored_vals[3] = {0,0,0}; 
    while(1) {
        // --- 1. 電源端邏輯 (控制 LED) ---
        // 根據 A1_1 和 A1_2 的狀態決定 A2/A3/A4 哪個亮
        int a1_1 = io_read_pin(A1_1_GPIO);
        int a1_2 = io_read_pin(A1_2_GPIO);
        
        // 邏輯表 (依據你的硬體設計)
        if (a1_1 && a1_2) { 
            // 兩者皆高 -> 自動模式 (A2 亮)
            io_write_pin(A2_GPIO,1); io_write_pin(A3_GPIO,0); io_write_pin(A4_GPIO,0); 
        }
        else if (a1_1) { 
            // 僅 A1_1 高 -> 手動模式 (A3 亮)
            io_write_pin(A2_GPIO,0); io_write_pin(A3_GPIO,1); io_write_pin(A4_GPIO,0); 
        }
        else if (a1_2) { 
            // 僅 A1_2 高 -> 搖桿模式 (A4 亮)
            io_write_pin(A2_GPIO,0); io_write_pin(A3_GPIO,0); io_write_pin(A4_GPIO,1); 
        }
        else { 
            // 全滅
            io_write_pin(A2_GPIO,0); io_write_pin(A3_GPIO,0); io_write_pin(A4_GPIO,0); 
        }

        // --- 2. 選擇端邏輯 (B系列) ---
        // 僅在手動模式 (A3 亮時) 運作
        if (io_read_pin(A3_GPIO)) {
            // 切換開關 B4 決定讀取哪個電位器
            int val = io_read_pin(B4_GPIO) ? read_pot_raw(3) : read_pot_raw(2);
            
            // 三檔位開關 B1 決定儲存目標索引 (0, 1, 2)
            int idx = (io_read_pin(B1_1_GPIO)==0 && io_read_pin(B1_2_GPIO)==0) ? 0 :
                      (io_read_pin(B1_1_GPIO)==0 && io_read_pin(B1_2_GPIO)==1) ? 1 : 2;
            
            // 點動開關 B5 按下時 -> 儲存數值並鳴叫蜂鳴器
            if (io_read_pin(B5_GPIO)) {
                stored_vals[idx] = val;
                io_write_pin(B6_GPIO, 1);       // 蜂鳴器響
                vTaskDelay(pdMS_TO_TICKS(100)); // 響 100ms
                io_write_pin(B6_GPIO, 0);       // 蜂鳴器停
            }
        }
        
        // 每 200ms 更新一次邏輯
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// 主程式入口
void app_main(void)
{
    // 1. 初始化 NVS (系統儲存區)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. 載入儲存的設定 (WiFi/IP)
    load_settings();

    // 3. 掛載 SPIFFS (網頁檔案系統)
    // 確保根目錄有 spiffs_image 資料夾且內含 index.html
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = "storage",
      .max_files = 5,
      .format_if_mount_failed = true
    };
    esp_vfs_spiffs_register(&conf);

    // 4. 初始化網路介面層
    esp_netif_init();
    esp_event_loop_create_default();

    // 5. 初始化硬體
    io_init();
    comms_uart_init();

    // 6. WiFi 初始化 (優先嘗試 STA，失敗則轉 AP)
    bool connected = wifi_init_sta();
    if (!connected) {
        ESP_LOGW(TAG, "WiFi Failed, Starting AP Mode...");
        wifi_init_ap();
    }

    // 7. 啟動網頁伺服器
    start_webserver();

    if(connected) ESP_LOGI(TAG, "System Ready (STA Mode)");
    else ESP_LOGW(TAG, "System in RESCUE Mode (AP: 192.168.4.1)");

    // 8. 啟動狀態邏輯任務
    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
}