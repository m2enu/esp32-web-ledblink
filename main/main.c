#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "tcpip_adapter.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "string.h"

/*! <!-- wifi settings {{{1 -->
 * @brief set the ssid and password via "make menuconfig"
 */
#define WIFI_SSID   CONFIG_WIFI_SSID
#define WIFI_PWD    CONFIG_WIFI_PASSWORD

/*! <!-- GPIO settings {{{1 -->
 * @brief set the GPIO port via "make menuconfig"
 */
#define GPIO_LED    CONFIG_BLINK_GPIO

/*! <!-- Global members {{{1 -->
 * @brief
 */
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const static char http_index_hml[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-type: text/html\r\n\r\n"
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<meta name='viewport' content='initial-scale=1.5'>\n"
    "</head>\n"
    "<body>\n"
    "<form method='get'>\n"
    "ESP-WROOM-32<br>\n"
    "Wi-Fi  LED  Switch<br><br>\n"
    "<input type='submit' name=0 value='ON' style='background-color:#88ff88; color:red;'>\n"
    "<input type='submit' name=1 value='OFF' style='background-color:black; color:white;'>\n"
    "</form>\n"
    "</body>\n"
    "</html>\n";

/*! <!-- event_handler {{{1 -->
 * @brief
 * @param ctx
 * @param event
 */
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        printf("got ip\n");
        printf("ip     : " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.ip));
        printf("netmask: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.netmask));
        printf("gw     : " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.gw));
        printf("\n");
        fflush(stdout);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

/*! <!-- initialise_wifi {{{1 -->
 * @brief
 */
static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PWD,
            .bssid_set = false
        }
    };

    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

/*! <!-- http_server_netconn_serve {{{1 -->
 * @brief
 * @param conn
 */
static void http_server_netconn_serve(struct netconn *conn)
{
    struct netbuf *inbuf;
    char *buf;
    u16_t buflen;
    err_t err;

    /* Read the data from the port, blocking if nothing yet there.
     We assume the request (the part we care about) is in one netbuf */
    err = netconn_recv(conn, &inbuf);

    if (err == ERR_OK) {
        netbuf_data(inbuf, (void**)&buf, &buflen);

        // strncpy(_mBuffer, buf, buflen);

        /* Is this an HTTP GET command? (only check the first 5 chars, since
         there are other formats for GET, and we're keeping it very simple )*/
        printf("buffer = %s \n", buf);
        if (buflen>=9 &&
            buf[0]=='G' && buf[1]=='E' && buf[2]=='T' && buf[3]==' ' && buf[4]=='/' ) {
            /* Send the HTML header, page
             * subtract 1 from the size, since we dont send the \0 in the string
             * NETCONN_NOCOPY: our data is const static, so no need to copy it
             */
            netconn_write(conn, http_index_hml, sizeof(http_index_hml)-1, NETCONN_NOCOPY);

            // LED control
            if(buf[5]=='?' && buf[6]=='1' && buf[7]=='=' && buf[8]=='O' && buf[9]=='F') {
                gpio_set_level(GPIO_LED, 0);
            }
            else if(buf[5]=='?' && buf[6]=='0' && buf[7]=='=' && buf[8]=='O' && buf[9]=='N') {
                gpio_set_level(GPIO_LED, 1);
            }
        }

    }
    /* Close the connection (server closes in HTTP) */
    netconn_close(conn);

    /* Delete the buffer (netconn_recv gives us ownership,
     so we have to make sure to deallocate the buffer) */
    netbuf_delete(inbuf);
}

/*! <!-- http_server {{{1 -->
 * @brief
 * @param pvParameters
 */
static void http_server(void *pvParameters)
{
    struct netconn *conn, *newconn;
    err_t err;
    conn = netconn_new(NETCONN_TCP);
    netconn_bind(conn, NULL, 80);
    netconn_listen(conn);
    do {
        err = netconn_accept(conn, &newconn);
        if (err == ERR_OK) {
            http_server_netconn_serve(newconn);
            netconn_delete(newconn);
        }
    } while(err == ERR_OK);
    netconn_close(conn);
    netconn_delete(conn);
}

/*! <!-- app_main {{{1 -->
 * @brief
 */
int app_main(void)
{
    nvs_flash_init();
    initialise_wifi();

    gpio_set_direction(GPIO_LED, GPIO_MODE_OUTPUT);
    xTaskCreate(&http_server, "http_server", 2048, NULL, 5, NULL);
    return 0;
}

// end of file {{{1
// vim:ft=c:et:nowrap:fdm=marker
