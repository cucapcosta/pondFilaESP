#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "cJSON.h"

#define WIFI_SSID        "MyNetwork" //N vou commitar minha rede de casa né :P
#define WIFI_PASSWORD    "MyPassword"
#define SERVER_URL       "http://192.168.1.100:3420/add"
#define SENSOR_ID        "ESP32-001"
#define SEND_INTERVAL_MS 5000

static const char *TAG = "sensor";


#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_eg;
static int s_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry++;
        ESP_LOGW(TAG, "WiFi desconectado, tentativa %d...", s_retry);
        esp_wifi_connect();  // reconecta sempre
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando WiFi \"%s\"...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi conectado!");
}

/*
 * Para usar um sensor real (ex: LM35 no GPIO34)
 *   #include "driver/adc.h"
 *
 *   // Na inicializacao (chamar uma vez):
 *   // adc1_config_width(ADC_WIDTH_BIT_12);
 *   // adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12);
 *
 *   // Na leitura:
 *   // int raw = adc1_get_raw(ADC1_CHANNEL_6);  // GPIO34
 *   // float voltage = (raw / 4095.0) * 3.3;
 *   // float temp = voltage * 100.0;  // LM35: 10mV/°C
 *   // return temp;
 */
static float mock_temperature(void)
{
    return 20.0f + ((float)(esp_random() % 1500) / 100.0f);  // 20.0 ~ 35.0
}

static float mock_humidity(void)
{
    return 30.0f + ((float)(esp_random() % 5000) / 100.0f);  // 30.0 ~ 80.0
}


static esp_err_t http_post(const char *type, const char *unit, float value)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "SensorID", SENSOR_ID);
    cJSON_AddStringToObject(root, "Timestamp", "2026-01-01T00:00:00Z");  // mock
    cJSON_AddStringToObject(root, "Type", type);
    cJSON_AddStringToObject(root, "Unit", unit);
    cJSON_AddBoolToObject(root, "IsDiscrete", 0);
    cJSON_AddNumberToObject(root, "Value", (double)value);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        ESP_LOGE(TAG, "Erro ao montar JSON");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "POST %s -> %s", SERVER_URL, body);

    // Envia com retry simples
    esp_err_t result = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        esp_http_client_config_t cfg = {
            .url = SERVER_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 5000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP %d OK", status);
            esp_http_client_cleanup(client);
            result = ESP_OK;
            break;
        }

        ESP_LOGW(TAG, "Falha (%d/3): %s", i + 1, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    cJSON_free(body);
    return result;
}


void app_main(void)
{
    // Init NVS (necessario pro WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Conecta WiFi
    wifi_init_sta();

    // Loop: le sensor mockado, envia via HTTP
    ESP_LOGI(TAG, "Iniciando envio a cada %d ms...", SEND_INTERVAL_MS);
    while (1) {
        float temp = mock_temperature();
        float hum  = mock_humidity();
        ESP_LOGI(TAG, "Leitura: temp=%.1f C  hum=%.1f %%", temp, hum);

        http_post("temperature", "C", temp);
        http_post("humidity", "%", hum);

        vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS));
    }
}
