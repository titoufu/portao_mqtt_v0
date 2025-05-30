#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "config.h"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0


static int last_state = -1;
esp_mqtt_client_handle_t mqtt_client = NULL;

// MQTT event handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado ao broker MQTT");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Desconectado do broker MQTT");
        break;
    default:
        break;
    }
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi desconectado, tentando reconectar...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi conectado, IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Inicializando WiFi...");
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// Função para publicar estado do portão via MQTT
static void publish_state(int state)
{
    const char *msg = (state == 0) ? "Fechado" : "Aberto";
    ESP_LOGI(TAG, "Portão: %s", msg);

    if (mqtt_client)
    {
        esp_mqtt_client_publish(mqtt_client, "casa/portao/estado", msg, 0, 1, 0);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // Configura GPIO reed
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << REED_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // Inicializa WiFi
    wifi_init();

    // Aguarda conexão WiFi
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // Inicializa MQTT
    mqtt_app_start();

    // Lê estado atual do reed
    int reed_state = gpio_get_level(REED_GPIO);
    last_state = reed_state;

    // Publica estado atual
    publish_state(reed_state);

    // Lógica para deep sleep:
    if (reed_state == 0) {
        // Portão fechado -> entra em deep sleep e acorda com mudança para aberto (alto)
        ESP_LOGI(TAG, "Portão fechado. Entrando em deep sleep, aguardando abertura...");
        esp_sleep_enable_ext0_wakeup(REED_GPIO, 1);  // acorda quando reed for 1 (alto)
        esp_deep_sleep_start();
    } else {
        // Portão aberto -> fica acordado, monitora mudanças
        ESP_LOGI(TAG, "Portão aberto. Mantendo dispositivo acordado.");

        while (true)
        {
            int state = gpio_get_level(REED_GPIO);
            if (state != last_state)
            {
                last_state = state;
                publish_state(state);

                // Se portão fechou, entra em deep sleep
                if (state == 0)
                {
                    ESP_LOGI(TAG, "Portão fechou durante monitoramento. Entrando em deep sleep...");
                    esp_sleep_enable_ext0_wakeup(REED_GPIO, 1);
                    esp_deep_sleep_start();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
