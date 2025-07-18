#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"   // 🔥 加這個才能用 MACSTR / MAC2STR
#include "driver/ledc.h"
#include "esp_err.h"
#include "mqtt_client.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "cJSON.h"
#include "protocomm_security.h"
//mosquitto_sub -i test_sub1 -t "emqx/esp32" -d  //利用電腦監聽指令


#define MESH_ID  {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
#define MQTT_BROKER    "mqtt://192.168.0.66:1883"  // 也可以改成 "mqtt://192.168.1.100"
#define MQTT_TOPIC_push     "emqx/esp32"
#define MQTT_TOPIC_read     "emqx/esp"
#define TAG "WIFI_mesh_root"

bool prov;      //檢測是否有憑證
bool is_ble_initialized =false;  //避免重複執行
wifi_config_t current_conf;  //wifi 存放處
static esp_mqtt_client_handle_t client;  // 全域變數，存 MQTT 客戶端句柄
//-----------------藍芽宣告區------------------------------
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = "abcd1234";  // Proof of Possession
    const char *service_name = "esp32_root";  // BLE 廣播名稱
    const char *service_key = NULL;  // 可設定密鑰
//--------------------------------------------------------

int value;
typedef struct {
    char name[32];
    mesh_addr_t addr;
} device_entry_t;

#define MAX_DEVICES 10
device_entry_t device_table[MAX_DEVICES];
int device_count = 0;
int light;

void analogWrite(uint8_t pin, uint32_t value)   //利用gpt製作類是arduino 控制pwm 
{
    static bool initialized = false;

    if (!initialized) {
        // 只初始化一次
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = LEDC_TIMER_0,
            .duty_resolution = LEDC_TIMER_8_BIT,  // 0~255
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ledc_timer_config(&ledc_timer);
        initialized = true;
    }

    ledc_channel_config_t ledc_channel = {
        .gpio_num   = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = value,
        .hpoint     = 0
    };
    ledc_channel_config(&ledc_channel);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, value);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void light_control()
{

    if(light == 1)
    {
        printf("亮");
        analogWrite(14,255);
        esp_mqtt_client_publish(client, MQTT_TOPIC_push, "on", 0, 0, 0);
        ESP_LOGI(TAG, "on");

    }
    else
    {
        printf("暗");
        analogWrite(14,0);
        esp_mqtt_client_publish(client, MQTT_TOPIC_push, "off", 0, 0, 0);
        ESP_LOGI(TAG, "off");
    }    

}

void add_device_to_table(const char *name, mesh_addr_t *addr) {
    if (device_count >= MAX_DEVICES) return;

    // 檢查是否已存在
    for (int i = 0; i < device_count; ++i) {
        if (memcmp(device_table[i].addr.addr, addr->addr, 6) == 0) return;
    }

    strncpy(device_table[device_count].name, name, sizeof(device_table[device_count].name));
    memcpy(&device_table[device_count].addr, addr, sizeof(mesh_addr_t));
    device_count++;

    printf("已登記裝置：%s - " MACSTR "\n", name, MAC2STR(addr->addr));
}






// MQTT 事件處理
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) 
{
    ESP_LOGI(TAG, "MQTT 事件 ID: %" PRId32, event_id);

    switch (event_id) 
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT 連線成功！");
            esp_mqtt_client_publish(client, MQTT_TOPIC_push, "Hello from ESP32!", 0, 0, 0);
            esp_mqtt_client_subscribe(client, MQTT_TOPIC_read, 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT 斷線！");
            break;


        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "收到 MQTT 訊息！");
            esp_mqtt_event_handle_t event = event_data;
            char topic[event->topic_len + 1];
            memcpy(topic, event->topic, event->topic_len);
            topic[event->topic_len] = 0;

            char data[event->data_len + 1];
            memcpy(data, event->data, event->data_len);
            data[event->data_len] = 0;

            ESP_LOGI(TAG, "📥 收到訊息 Topic: %s, Data: %s", topic, data);
            if (strcmp(data, "on") == 0) 
            {
            light = 1;
            light_control();
            } 
            else if (strcmp(data, "off") == 0) 
            {
                light = 0;
                light_control();
            }
            break;
        // default:
        //     ESP_LOGI(TAG, "其他 MQTT 事件: %" PRId32, event_id);
        //     break;
    }
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER,  // MQTT Broker 連線設定
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}


void root_recv_task(void *arg)
{
    mesh_addr_t from;
    mesh_data_t data;
    static uint8_t data_buf[300];

    memset(&data, 0, sizeof(mesh_data_t));
    data.data = data_buf;
    data.size = sizeof(data_buf);

    while (1) {
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, NULL, NULL, NULL);

        if (err == ESP_OK) {
            // 資料是 int？
            if (data.size == sizeof(int)) 
            {
                int value;
                memcpy(&value, data.data, sizeof(int));
                light = value;
                printf("Root: 收到 int = %d\n", value);
                light_control();

            } else {
                // 嘗試解析成 JSON
                char *json_str = (char *)data.data;
                cJSON *json = cJSON_Parse(json_str);

                if (json) {
                    cJSON *type = cJSON_GetObjectItem(json, "type");
                    if (type && strcmp(type->valuestring, "register") == 0) {
                        cJSON *name = cJSON_GetObjectItem(json, "name");
                        if (name) {
                            add_device_to_table(name->valuestring, &from);
                        }
                    } else {
                        printf("Root: 收到 JSON 非註冊：%s\n", json_str);
                    }

                    cJSON_Delete(json);
                } else {
                    // 非 JSON，一般處理
                    printf("Root: Received (%d bytes) from " MACSTR ": %s\n", 
                        data.size, MAC2STR(from.addr), (char *)data.data);
                }
            }

        } else {
            printf("Root: Failed to receive message, err=0x%x\n", err);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) //wifi事件處理函式(即時顯示wifi連線狀況)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "✅ Wi-Fi 連線成功!");
        mqtt_app_start(); 
        // mesh_init();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "⚠️ Wi-Fi 斷線，重新嘗試...");
        esp_wifi_connect();
    }
}


void mesh_init()  //mesh部分初始化
{
    mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
    uint8_t mesh_id[6] = MESH_ID;
    memcpy(mesh_cfg.mesh_id.addr, mesh_id, 6);

    mesh_cfg.mesh_ap.max_connection = 6;
    memcpy((char *)mesh_cfg.mesh_ap.password, "12345678", strlen("12345678"));

// 設定Router
    strcpy((char *)mesh_cfg.router.ssid, (char *)current_conf.sta.ssid);
    mesh_cfg.router.ssid_len = strlen((char *)current_conf.sta.ssid);  // 🔥 這行是重點
    memcpy(mesh_cfg.router.password, current_conf.sta.password, sizeof(mesh_cfg.router.password));



    mesh_cfg.channel = 0;
    mesh_cfg.allow_channel_switch = true;

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_cfg));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
     ESP_LOGI("MESH", "✅ Mesh started!");
}


static void wifi_init() //wifi初始化
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "🚀 Wi-Fi 初始化完成");
}
static void prov_event_handler(void *user_data, wifi_prov_cb_event_t event, void *event_data) 
{
    switch (event) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "📡 BLE Provisioning 開始...");
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "📥 接收到 Wi-Fi 設定 -> SSID: %s, 密碼: %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
           
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "✅ Provisioning 成功");
            break;
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "⚡ Provisioning 結束");
            wifi_prov_mgr_deinit();
            break;
        default:
            break;
    }
}

void blu_prov()
{
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&prov));  // <<== 加上這行！正確讀取狀態

    if (!prov) {
        ESP_LOGI(TAG, "🔵 沒憑證，啟動 BLE 配對...");
        if (!is_ble_initialized) {
            wifi_prov_mgr_config_t config = {
                .scheme = wifi_prov_scheme_ble,
                .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
            };
            ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
            is_ble_initialized = true;
        }
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));
    } else 
    {
        ESP_LOGI(TAG, "📶 已有憑證，直接連線 Wi-Fi");
        
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &current_conf);
    if (err == ESP_OK)
    {
    ESP_LOGI("WIFI", "✅ Connected to SSID: %s", (char *)current_conf.sta.ssid);
    ESP_LOGI("WIFI", "🔐 Password used : %s", (char *)current_conf.sta.password);
    mesh_init();
    }
    }
}
void nvs_init()
{
    ESP_LOGI(TAG, "初始化nvs");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}



void app_main(void)
{
    nvs_init();
    wifi_init();
    blu_prov();
    xTaskCreate(root_recv_task, "root_recv_task", 4096, NULL, 5, NULL);
    // while(1)
    // {
    //     int i=rand();
    //     esp_mqtt_client_publish(client, MQTT_TOPIC, "on", 0, 0, 0);
    //     vTaskDelay(pdMS_TO_TICKS(500));
    //     ESP_LOGI(TAG, "送了狀態");
    //     i++;
    // }
}
