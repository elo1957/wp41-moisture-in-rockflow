/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include <sht3x.h>
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "esp_spi_flash.h"
#include "driver/spi_master.h"
#include "esp_types.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"

#define SDA_GPIO 21 // green
#define SCL_GPIO 22
#define ADDR SHT3X_I2C_ADDR_GND

#define FSPI 1 // SPI bus attached to the flash (can use the same data lines but different SS)
#define HSPI 2 // SPI bus normally mapped to pins 12 - 15, but can be matrixed to any pins
#define VSPI 3 // SPI bus normally attached to pins 5, 18, 19 and 23, but can be matrixed to any pins
// #define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
static const char *TAG = "adc_spi_MyModule";
#define STACK_SIZE 2048
#define tskIDLE_PRIORITY ((UBaseType_t)5U)
// #define portTICK_PERIOD_MS  10       //33
/*      HSPI VSPI
GPIO Number
CS0*	15	5
SCLK	14	18
MISO	12	19
MOSI	13	22
QUADWP	2	22
QUADHD	4	21*/

#define PIN_NUM_CLK 18  //  31 6 //
#define PIN_NUM_MISO 19 //   D2 9//
#define PIN_NUM_MOSI 23 // D3 10//
#define PIN_NUM_CS 5    // CMD 11//

static sht3x_t dev, dev1; //
static EventGroupHandle_t mqtt_event_group;
static esp_mqtt_client_handle_t client = NULL;
const static int CONNECTED_BIT = BIT0;
char payl[146];
SemaphoreHandle_t print_mux = NULL;
spi_device_handle_t handle;
spi_bus_config_t buscfg = {
    .miso_io_num = PIN_NUM_MISO,
    .mosi_io_num = PIN_NUM_MOSI,
    .sclk_io_num = PIN_NUM_CLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .flags = SPICOMMON_BUSFLAG_MASTER

};
spi_device_interface_config_t devcfg = {
    .address_bits = 0,
    .command_bits = 0,
    .dummy_bits = 0,
    //.mode = 0,
    .mode = 0,
    .duty_cycle_pos = 0,
    .cs_ena_posttrans = 0,
    .cs_ena_pretrans = 0,
    .input_delay_ns = 0,
    .clock_speed_hz = 1000000, // devisor

    .spics_io_num = PIN_NUM_CS, // CS pin
    .queue_size = 1             // We want to be able to queue 7 transactions at a time

};

// SDA_GPIO, SCL_GPIO
i2c_config_t conf_slave = {
    .sda_io_num = SDA_GPIO, // select GPIO specific to your project
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_io_num = SCL_GPIO, // select GPIO specific to your project
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
};
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id = 0;

    // your_context_t *context = event->context;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        xEventGroupSetBits(mqtt_event_group, CONNECTED_BIT);

        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        // 1 m batch 1802302
        // 1.5 m batch 1802328
        // reference humidty and temperature  1802352
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_subscribe(client, "channels/1802352/subscribe/fields", 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
        .port = 1883,

        // batch 1m
        .client_id = "DAMsFwwINjsYMgcvFTguCxQ",
        .username = "DAMsFwwINjsYMgcvFTguCxQ",
        .password = ""
        // batch1.5m
        /* .client_id="OCQiDBY4Ej0LOzQ6ASkYHQg",
         .username="OCQiDBY4Ej0LOzQ6ASkYHQg",
         .password=""*/
        // reference humidity and temperature
        /*  .client_id="DggINQkWAA0bERsQDT0mLRs",
         .username="DggINQkWAA0bERsQDT0mLRs",
         .password=""*/
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0)
    {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128)
        {
            int c = fgetc(stdin);
            if (c == '\n')
            {
                line[count] = '\0';
                break;
            }
            else if (c > 0 && c < 127)
            {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    }
    else
    {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    // esp_mqtt_client_start(client);
}

// void *pvParameters
static void test_spi_task(void *pvParameters)
{
    uint8_t nr = 0, d = 0, d1 = 0;
    char res[40];
    unsigned int data[6], msg_id = 0;
    unsigned int ch0 = 0, ch1 = 0, ch2 = 0, ch3 = 0, ch4 = 0, ch5 = 0;
    unsigned int lastAdcData = 0;
    uint8_t MSB = 0;
    uint8_t LSB = 0;
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = 100;
    esp_err_t ret;
    // spi_device_handle_t spi;

    // Initialize the SPI bus
    ret = spi_bus_initialize(VSPI_HOST, &buscfg, 0);
    ESP_ERROR_CHECK(ret);
    // Attach the LCD to the SPI bus
    ret = spi_bus_add_device(VSPI_HOST, &devcfg, &handle);
    ESP_ERROR_CHECK(ret);

    spi_transaction_t trans_desc;

    uint8_t tx_data[4]; // rx_data[4];
    memset(&trans_desc, 0, sizeof(trans_desc));

    trans_desc.flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA;
    trans_desc.length = 8 * 3;
    trans_desc.rxlength = 8 * 3;
    tx_data[1] = 0x00; // d1
    trans_desc.tx_data[2] = 0x00;
    while (1)
    {
        xSemaphoreTake(print_mux, portMAX_DELAY);
        xLastWakeTime = xTaskGetTickCount();

        while (nr < 6)
        {

            switch (nr)
            {
            case 0:
                d = 6;
                d1 = 0;
                break;
            case 1:
                d1 = 0x40;
                break;
            case 2:
                d1 = 0x80;
                break;
            case 3:
                d1 = 0xC0;
                break;
            case 4:
                d = 7;
                d1 = 0;
                break;
            case 5:
                d1 = 0x40;
                break;
                // case 6:  d1=0x80;break;
                // case 7: d1=0xC0;break;
            default:
                break;
            }

            trans_desc.tx_data[0] = d;
            trans_desc.tx_data[1] = d1;
            trans_desc.tx_data[2] = 0;

            for (int i = 0; i < 10; i++)
            {
                ESP_LOGI(TAG, "... Transmitting.");
                esp_err_t ret = (spi_device_polling_transmit(handle, &trans_desc));
                assert(ret == ESP_OK);

                MSB = trans_desc.rx_data[1];
                LSB = trans_desc.rx_data[2];

                // printf("data , kanal %u, %02X,%02X:",nr,trans_desc.rx_data[1],trans_desc.rx_data[2]);
                lastAdcData = ((((MSB & 15) << 8) + LSB));
                data[nr] = lastAdcData + data[nr];
            }
            data[nr] = data[nr] / 10;
            sprintf(res, "  ADC %u,%d :", nr, data[nr]);
            printf("adc %s", res); //&field7=%d&field8=%d
            if (nr == 0)
                ch0 = data[0];
            if (nr == 1)
                ch1 = data[1];
            if (nr == 2)
                ch2 = data[2];
            if (nr == 3)
                ch3 = data[3];
            if (nr == 4)
                ch4 = data[4];
            if (nr == 5)
                ch5 = data[5];
            if (ch0 > 3550)
                ch0 = 3510;
            if (ch1 > 3550)
                ch1 = 3510;
            if (ch2 > 3550)
                ch2 = 3510;
            if (ch3 > 3550)
                ch3 = 3510;
            if (ch4 > 3550)
                ch4 = 3510;
            if (ch5 > 3550)
                ch5 = 3510;
            data[nr] = 0;
            nr++;
            lastAdcData = 0;
        }

        nr = 0;
        sprintf(payl, "field1=%d&field2=%d&field3=%d&field4=%d&field5=%d&field6=%d&status=MQTTPUBLISH", ch0, ch1, ch2, ch3, ch4, ch5);
        ESP_LOGI(TAG, "... done.");

        esp_mqtt_client_start(client);
        ESP_LOGI(TAG, "Note free memory: %d bytes", esp_get_free_heap_size());
        xEventGroupWaitBits(mqtt_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
        // 1607623 experimenterkalibrering
        msg_id = esp_mqtt_client_publish(client, "channels/1802302/publish", payl, 0, 0, 0);
        // batch 1 m 1802302
        // batch 1.5 1802328
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        esp_mqtt_client_stop(client);
        xSemaphoreGive(print_mux);
        vTaskDelay(pdMS_TO_TICKS(60000)); // 60 sekunder
    }

    vTaskDelete(NULL);
}

 * User task that triggers a measurement every 5 seconds. Due to power
 * efficiency reasons it uses *single shot* mode. In this example it starts the
 * measurement, waits for the results and fetches the results using separate
 * functions
 */
void Measuretask(void *pvParameters)
 {

     float temperature = 0, Ltemperature = 0, Rtemperature = 0;
     float humidity = 0, Lhumidity = 0, Rhumidity = 0;
     char res[130];

     TickType_t last_wakeup = xTaskGetTickCount();
     ESP_ERROR_CHECK(i2cdev_init());
     memset(&dev, 0, sizeof(sht3x_t));

     ESP_ERROR_CHECK(sht3x_init_desc(&dev, 0, ADDR, SDA_GPIO, SCL_GPIO));
     ESP_ERROR_CHECK(sht3x_init(&dev));
     // get the measurement duration for high repeatability;
     uint8_t duration = sht3x_get_measurement_duration(SHT3X_HIGH);

     uint8_t msg_id;
     while (1)
     {
         xSemaphoreTake(print_mux, portMAX_DELAY);

         esp_mqtt_client_start(client);

         last_wakeup = xTaskGetTickCount();

         memset(&dev, 0, sizeof(sht3x_t));
         ESP_ERROR_CHECK(sht3x_init_desc(&dev, 0, ADDR, SDA_GPIO, SCL_GPIO));

         ESP_ERROR_CHECK(sht3x_init(&dev));

         for (int i = 0; i < 10; i++)
         {
             ESP_ERROR_CHECK(sht3x_start_measurement(&dev, SHT3X_SINGLE_SHOT, SHT3X_HIGH));

             // Wait until measurement is ready (constant time of at least 30 ms
             // or the duration returned from *sht3x_get_measurement_duration*).
             vTaskDelay(duration);

             // retrieve the values and do something with them
             ESP_ERROR_CHECK(sht3x_get_results(&dev, &temperature, &humidity));
             Ltemperature = temperature;
             Rtemperature = Ltemperature + Rtemperature;
             Lhumidity = humidity;
             Rhumidity = Lhumidity + Rhumidity;
         }
         temperature = Rtemperature / 10;
         Rtemperature = 0;
         humidity = Rhumidity / 10;
         Rhumidity = 0;
         //   sprintf(res,"field1=%.2f&field2=%.2f&status=MQTTPUBLISH", humidity, temperature);
         printf("SHT3x Sensor: %.2f °C, %.2f %%\n", temperature, humidity);
         //  msg_id = esp_mqtt_client_publish(client1, "channels/1768048/publish", res, 0, 0, 0);
         sprintf(res, "field7=%.2f&field8=%.2f&status=MQTTPUBLISH", humidity, temperature);
         // eksperimenter 1607623
         printf(res);
         msg_id = esp_mqtt_client_publish(client, "channels/1802302/publish", res, 0, 0, 0);
         // 1m batch 1802302
         // 1.5 m batch 1802328
         // reference humidity and temperature 1802352
         ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

         esp_mqtt_client_stop(client);
         xSemaphoreGive(print_mux);

         vTaskDelayUntil(&last_wakeup, pdMS_TO_TICKS(900000)); // 15 min. interval
         last_wakeup = xTaskGetTickCount();
     }
 }
 void app_main(void)
 {
     printf("Start logger\n");

     print_mux = xSemaphoreCreateMutex();
     ESP_LOGI(TAG, "[APP] Startup..");
     ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
     ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

     esp_log_level_set("*", ESP_LOG_INFO);
     esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
     esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
     esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
     esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
     esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
     esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

     ESP_ERROR_CHECK(nvs_flash_init());
     ESP_ERROR_CHECK(esp_netif_init());
     ESP_ERROR_CHECK(esp_event_loop_create_default());
     mqtt_event_group = xEventGroupCreate();

     */
         ESP_ERROR_CHECK(example_connect());

     ESP_LOGD(TAG, ">> test_spi_task");

     mqtt_app_start();
     xTaskCreate(test_spi_task, "test_spi_task", 4000, NULL, 5, NULL);
     xTaskCreate(Measuretask, "Measuretask", 4000, NULL, 10, NULL);
 }
