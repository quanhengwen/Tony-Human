#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "nvs.h"
#include "Json_parse.h"
#include "E2prom.h"
#include "Bluetooth.h"
#include "Human.h"
#include "Led.h"
#include "Smartconfig.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "freertos/event_groups.h"
#include "w5500_driver.h"
#include "my_base64.h"
#include "Http.h"

SemaphoreHandle_t xMutex_Http_Send = NULL;
SemaphoreHandle_t Binary_Http_Send = NULL;
SemaphoreHandle_t Binary_Heart_Send = NULL;
SemaphoreHandle_t Binary_Human_Send = NULL;

// extern uint8_t data_read[34];

static char *TAG = "HTTP";
uint32_t HTTP_STATUS = HTTP_KEY_GET;
uint8_t post_status = POST_NOCOMMAND;

static char build_heart_url[256];
static char build_human_url[512];

TaskHandle_t httpHandle = NULL;
esp_timer_handle_t http_timer_suspend_p = NULL;

void timer_heart_cb(void *arg);
esp_timer_handle_t timer_heart_handle = NULL; //定时器句柄
esp_timer_create_args_t timer_heart_arg = {
    .callback = &timer_heart_cb,
    .arg = NULL,
    .name = "Heart_Timer"};

void timer_human_cb(void *arg);
esp_timer_handle_t timer_human_handle = NULL; //定时器句柄
esp_timer_create_args_t timer_human_arg = {
    .callback = &timer_human_cb,
    .arg = NULL,
    .name = "Human_Timer"};

int32_t wifi_http_send(char *send_buff, uint16_t send_size, char *recv_buff, uint16_t recv_size)
{
    // printf("wifi http send start!\n");
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int32_t s = 0, r = 0;

    int err = getaddrinfo((const char *)WEB_SERVER, "80", &hints, &res); //step1：DNS域名解析

    if (err != 0 || res == NULL)
    {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return -1;
    }

    /* Code to print the resolved IP.
		Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    s = socket(res->ai_family, res->ai_socktype, 0); //step2：新建套接字
    if (s < 0)
    {
        ESP_LOGE(TAG, "... Failed to allocate socket. err:%d", s);
        close(s);
        freeaddrinfo(res);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return -1;
    }
    ESP_LOGI(TAG, "... allocated socket");

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) //step3：连接IP
    {
        ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return -1;
    }

    ESP_LOGI(TAG, "... connected");
    freeaddrinfo(res);

    ESP_LOGD(TAG, "http_send_buff send_buff: %s\n", (char *)send_buff);
    if (write(s, (char *)send_buff, send_size) < 0) //step4：发送http包
    {
        ESP_LOGE(TAG, "... socket send failed");
        close(s);
        vTaskDelay(4000 / portTICK_PERIOD_MS);
        return -1;
    }
    ESP_LOGI(TAG, "... socket send success");
    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, //step5：设置接收超时
                   sizeof(receiving_timeout)) < 0)
    {
        ESP_LOGE(TAG, "... failed to set socket receiving timeout");
        close(s);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return -1;
    }
    ESP_LOGI(TAG, "... set socket receiving timeout success");

    /* Read HTTP response */

    bzero((uint16_t *)recv_buff, recv_size);
    r = read(s, (uint16_t *)recv_buff, recv_size);
    ESP_LOGD(TAG, "r=%d,activate recv_buf=%s\r\n", r, (char *)recv_buff);
    close(s);
    // printf("http send end!\n");
    return r;
}

int32_t http_send_buff(char *send_buff, uint16_t send_size, char *recv_buff, uint16_t recv_size)
{
    xSemaphoreTake(xMutex_Http_Send, -1);
    // xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
    //                     false, true, -1); //等网络连接

    int32_t ret;
    if (LAN_DNS_STATUS == 1)
    {
        printf("lan send!!!\n");
        ret = lan_http_send(send_buff, send_size, recv_buff, recv_size);
        // printf("lan_http_send return :%d\n", ret);
        xSemaphoreGive(xMutex_Http_Send);
        return ret;
    }

    else
    {
        printf("wifi send!!!\n");
        ret = wifi_http_send(send_buff, send_size, recv_buff, recv_size);
        xSemaphoreGive(xMutex_Http_Send);
        return ret;
    }
}

void http_get_task(void *pvParameters)
{
    xSemaphoreGive(Binary_Http_Send); //先发送一次

    while (1)
    {
        //需要把数据发送到平台
        xSemaphoreTake(Binary_Http_Send, (fn_dp * 1000) / portTICK_PERIOD_MS);
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                            false, true, -1);
        printf("Http send !\n");
        http_send_mes();
    }
}

void send_human_task(void *arg)
{
    char recv_buf[1024] = {0};
    while (1)
    {
        xSemaphoreTake(Binary_Human_Send, -1);
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, -1); //等网络连接

        creat_json *pCreat_json1 = malloc(sizeof(creat_json)); //为 pCreat_json1 分配内存  动态内存分配，与free() 配合使用
        //创建POST的json格式
        create_http_json(pCreat_json1, 1);

        sprintf(build_human_url, "POST http://%s/update.json?api_key=%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: dalian urban ILS1\r\nContent-Length:%d\r\n\r\n%s",
                WEB_SERVER,
                ApiKey,
                WEB_SERVER,
                pCreat_json1->creat_json_c,
                pCreat_json1->creat_json_b);
        free(pCreat_json1);

        printf("Human send :\n%s!\n", build_human_url);
        if (http_send_buff(build_human_url, 1024, recv_buf, 1024) > 0)
        {
            // printf("解析返回数据！\n");
            ESP_LOGI(TAG, "mes recv:%s", recv_buf);
            if (parse_objects_http_respond(strchr(recv_buf, '{')))
            {
                Led_Status = LED_STA_WORK;
            }
            else
            {
                Led_Status = LED_STA_ACTIVE_ERR;
            }
        }
        else
        {
            Led_Status = LED_STA_WIFIERR;
            // printf("send return : %d \n", ret);
        }
    }
}

void send_heart_task(void *arg)
{
    char recv_buf[1024] = {0};

    while (1)
    {
        xSemaphoreTake(Binary_Heart_Send, -1);
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, -1); //等网络连接
        printf("Heart send !\n");
        if ((http_send_buff(build_heart_url, 256, recv_buf, 1024)) > 0)
        {
            ESP_LOGI(TAG, "hart recv:%s", recv_buf);
            if (parse_objects_heart(strchr(recv_buf, '{')))
            {
                //successed
                Led_Status = LED_STA_WORK;
            }
            else
            {
                Led_Status = LED_STA_ACTIVE_ERR;
            }
        }
        else
        {
            Led_Status = LED_STA_WIFIERR;
            printf("hart recv 0!\r\n");
        }
    }
}

//定时发送心跳包
void timer_heart_cb(void *arg)
{
    xSemaphoreGive(Binary_Heart_Send);
}

void timer_human_cb(void *arg)
{
    xSemaphoreGive(Binary_Human_Send);
}

//激活流程
int32_t http_activate(void)
{
    char build_http[256];
    char recv_buf[1024];

    sprintf(build_http, "GET http://%s/products/%s/devices/%s/activate\r\n\r\n", WEB_SERVER, ProductId, SerialNum);
    //http.HTTP_VERSION10, http.HOST, http.USER_AHENT, http.ENTER);

    ESP_LOGI(TAG, "build_http=%s\n", build_http);

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, -1); //等网络连接

    if (http_send_buff(build_http, 256, recv_buf, 1024) < 0)
    {
        Led_Status = LED_STA_WIFIERR;
        return 101;
    }
    else
    {
        ESP_LOGI(TAG, "active recv:%s", recv_buf);
        if (parse_objects_http_active(strchr(recv_buf, '{')))
        {
            Led_Status = LED_STA_WORK;
            return 1;
        }
        else
        {
            Led_Status = LED_STA_ACTIVE_ERR;
            return 102;
        }
    }
}

uint8_t Last_Led_Status;

void http_send_mes(void)
{
    int ret = 0;

    if (Led_Status != LED_STA_SEND) //解决两次发送间隔过短，导致LED一直闪烁
    {
        Last_Led_Status = Led_Status;
    }
    Led_Status = LED_STA_SEND;

    char recv_buf[1024] = {0};
    char build_po_url[512] = {0};
    char build_po_url_json[1024] = {0};
    char NET_INFO[64] = {0};

    if (LAN_DNS_STATUS == 1)
    {
        sprintf(NET_INFO, "&net=ethernet");
    }

    creat_json *pCreat_json1 = malloc(sizeof(creat_json)); //为 pCreat_json1 分配内存  动态内存分配，与free() 配合使用
    //创建POST的json格式
    create_http_json(pCreat_json1, 0);

    if (post_status == POST_NOCOMMAND) //无commID
    {
        sprintf(build_po_url, "POST http://%s/update.json?api_key=%s&metadata=true&firmware=%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: dalian urban ILS1\r\nContent-Length:%d\r\n\r\n",
                WEB_SERVER,
                ApiKey,
                FIRMWARE,
                WEB_SERVER,
                pCreat_json1->creat_json_c);
        // sprintf(build_po_url, "%s%s%s%s%s%s%s%s%s%s%s%s%d%s", http.POST, http.POST_URL1, ApiKey, http.POST_URL_METADATA, http.POST_URL_FIRMWARE, FIRMWARE, http.POST_URL_SSID, NET_NAME,
        //         http.HTTP_VERSION11, http.HOST, http.USER_AHENT, http.CONTENT_LENGTH, pCreat_json1->creat_json_c, http.ENTER);
    }
    else
    {
        post_status = POST_NOCOMMAND;

        sprintf(build_po_url, "POST http://%s/update.json?api_key=%s&metadata=true&firmware=%s&command_id=%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: dalian urban ILS1\r\nContent-Length:%d\r\n\r\n",
                WEB_SERVER,
                ApiKey,
                FIRMWARE,
                mqtt_json_s.mqtt_command_id,
                WEB_SERVER,
                pCreat_json1->creat_json_c);

        // sprintf(build_po_url, "%s%s%s%s%s%s%s%s%s%s%s%s%d%s", http.POST, http.POST_URL1, ApiKey, http.POST_URL_METADATA, http.POST_URL_SSID, NET_NAME, http.POST_URL_COMMAND_ID, mqtt_json_s.mqtt_command_id,
        //         http.HTTP_VERSION11, http.HOST, http.USER_AHENT, http.CONTENT_LENGTH, pCreat_json1->creat_json_c, http.ENTER);
    }

    sprintf(build_po_url_json, "%s%s", build_po_url, pCreat_json1->creat_json_b);

    // printf("JSON_test = : %s\n", pCreat_json1->creat_json_b);

    free(pCreat_json1);
    printf("build_po_url_json =\r\n%s\r\n build end \r\n", build_po_url_json);

    //发送并解析返回数据
    /***********調用函數發送***********/

    if (http_send_buff(build_po_url_json, 1024, recv_buf, 1024) > 0)
    {
        // printf("解析返回数据！\n");
        ESP_LOGI(TAG, "mes recv:%s", recv_buf);
        if (parse_objects_http_respond(strchr(recv_buf, '{')))
        {
            Led_Status = LED_STA_WORK;
        }
        else
        {
            Led_Status = LED_STA_ACTIVE_ERR;
        }
    }
    else
    {
        Led_Status = LED_STA_WIFIERR;
        printf("send return : %d \n", ret);
    }
}

void initialise_http(void)
{
    xMutex_Http_Send = xSemaphoreCreateMutex();   //创建HTTP发送互斥信号
    Binary_Http_Send = xSemaphoreCreateBinary();  //http
    Binary_Heart_Send = xSemaphoreCreateBinary(); //心跳包
    Binary_Human_Send = xSemaphoreCreateBinary(); //人感

    esp_err_t err = esp_timer_create(&timer_human_arg, &timer_human_handle);
    err = esp_timer_create(&timer_heart_arg, &timer_heart_handle);

    while (http_activate() != 1) //激活
    {
        ESP_LOGE(TAG, "activate fail\n");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    //心跳包 ,激活成功后获取
    sprintf(build_heart_url, "GET http://%s/heartbeat?api_key=%s HTTP/1.0\r\nHost: %sUser-Agent: dalian urban ILS1\r\n\r\n",
            WEB_SERVER,
            ApiKey,
            WEB_SERVER);
    printf("build_heart_url:%s", build_heart_url);
    xSemaphoreGive(Binary_Heart_Send);

    err = esp_timer_start_periodic(timer_heart_handle, 60 * 1000000); //创建定时器，单位us，定时60s
    if (err != ESP_OK)
    {
        printf("timer heart create err code:%d\n", err);
    }

    xTaskCreate(&http_get_task, "http_get_task", 8192, NULL, 6, &httpHandle);
    xTaskCreate(&send_heart_task, "send_heart_task", 8192, NULL, 5, NULL);
    xTaskCreate(&send_human_task, "send_human_task", 4096, NULL, 4, NULL);
}