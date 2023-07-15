#include "mwifi.h"
#include "mupgrade.h"
#include "mesh_mqtt_handle.h"
#include "mdf_common.h"
#include "dht11.h"

#define MY_ROUTER_SSID "ESPRESSIF"
#define MY_ROUTER_PASSWORD "20020806"
#define MY_MESH_ID "123456"
#define MY_MQTT_URL "mqtt://124.222.71.199:1883/mqtt"

static const char *TAG = "smart_agriculture";
esp_netif_t *sta_netif;

static void ota_task(void *arg)
{
    mdf_err_t ret = MDF_OK;
    uint8_t *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    char name[32] = {0x0};
    int total_size = 0;
    int start_time = 0;
    mupgrade_result_t upgrade_result = {0};
    mwifi_data_type_t data_type = {.communicate = MWIFI_COMMUNICATE_MULTICAST};

    /**
     * @note If you need to upgrade all devices, pass MWIFI_ADDR_ANY;
     *       If you upgrade the incoming address list to the specified device
     */
    // uint8_t dest_addr[][MWIFI_ADDR_LEN] = {{0x1, 0x1, 0x1, 0x1, 0x1, 0x1}, {0x2, 0x2, 0x2, 0x2, 0x2, 0x2},};
    uint8_t dest_addr[][MWIFI_ADDR_LEN] = {MWIFI_ADDR_ANY};

    /**
     * @brief In order to allow more nodes to join the mesh network for firmware upgrade,
     *      in the example we will start the firmware upgrade after 30 seconds.
     */
    vTaskDelay(10 * 1000 / portTICK_PERIOD_MS);
    esp_http_client_config_t config = {
        .url = (char *)arg,
        .transport_type = HTTP_TRANSPORT_UNKNOWN,
    };

    /**
     * @brief 1. Connect to the server
     */
    esp_http_client_handle_t client = esp_http_client_init(&config);
    MDF_ERROR_GOTO(!client, EXIT, "Initialise HTTP connection");

    start_time = xTaskGetTickCount();

    MDF_LOGI("Open HTTP connection: %s", config.url);

    /**
     * @brief First, the firmware is obtained from the http server and stored on the root node.
     */
    do
    {
        ret = esp_http_client_open(client, 0);

        if (ret != MDF_OK)
        {
            if (!esp_mesh_is_root())
            {
                goto EXIT;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
            MDF_LOGW("<%s> Connection service failed", mdf_err_to_name(ret));
        }
    } while (ret != MDF_OK);

    total_size = esp_http_client_fetch_headers(client);
    // sscanf(firmware_name, "%*[^/]//%*[^/]/%[^.]", name);

    if (total_size <= 0)
    {
        MDF_LOGW("Please check the address of the server");
        ret = esp_http_client_read(client, (char *)data, MWIFI_PAYLOAD_LEN);
        MDF_ERROR_GOTO(ret < 0, EXIT, "<%s> Read data from http stream", mdf_err_to_name(ret));

        MDF_LOGW("Recv data: %.*s", ret, data);
        goto EXIT;
    }

    /**
     * @brief 2. Initialize the upgrade status and erase the upgrade partition.
     */
    ret = mupgrade_firmware_init(name, total_size);
    MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> Initialize the upgrade status", mdf_err_to_name(ret));

    /**
     * @brief 3. Read firmware from the server and write it to the flash of the root node
     */
    for (ssize_t size = 0, recv_size = 0; recv_size < total_size; recv_size += size)
    {
        size = esp_http_client_read(client, (char *)data, MWIFI_PAYLOAD_LEN);
        MDF_ERROR_GOTO(size < 0, EXIT, "<%s> Read data from http stream", mdf_err_to_name(ret));

        if (size > 0)
        {
            /* @brief  Write firmware to flash */
            ret = mupgrade_firmware_download(data, size);
            MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> Write firmware to flash, size: %d, data: %.*s",
                           mdf_err_to_name(ret), size, size, data);
        }
        else
        {
            MDF_LOGW("<%s> esp_http_client_read", mdf_err_to_name(ret));
            goto EXIT;
        }
    }

    MDF_LOGI("The service download firmware is complete, Spend time: %ds",
             (xTaskGetTickCount() - start_time) * portTICK_RATE_MS / 1000);

    start_time = xTaskGetTickCount();

    /**
     * @brief 4. The firmware will be sent to each node.
     */
    ret = mupgrade_firmware_send((uint8_t *)dest_addr, sizeof(dest_addr) / MWIFI_ADDR_LEN, &upgrade_result);
    MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> mupgrade_firmware_send", mdf_err_to_name(ret));

    if (upgrade_result.successed_num == 0)
    {
        MDF_LOGW("Devices upgrade failed, unfinished_num: %d", upgrade_result.unfinished_num);
        goto EXIT;
    }

    MDF_LOGI("Firmware is sent to the device to complete, Spend time: %ds",
             (xTaskGetTickCount() - start_time) * portTICK_RATE_MS / 1000);
    MDF_LOGI("Devices upgrade completed, successed_num: %d, unfinished_num: %d", upgrade_result.successed_num, upgrade_result.unfinished_num);

    /**
     * @brief 5. the root notifies nodes to restart
     */
    const char *restart_str = "restart";
    ret = mwifi_root_write(upgrade_result.successed_addr, upgrade_result.successed_num,
                           &data_type, restart_str, strlen(restart_str), true);
    MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> mwifi_root_recv", mdf_err_to_name(ret));

EXIT:
    MDF_FREE(data);
    mupgrade_result_free(&upgrade_result);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

static void root_task(void *arg)
{
    mdf_err_t ret = MDF_OK;
    char *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    size_t size = MWIFI_PAYLOAD_LEN;
    mwifi_data_type_t data_type = {0};
    uint8_t src_addr[MWIFI_ADDR_LEN] = {0};

    mesh_mqtt_data_t *request = NULL;

    MDF_LOGI("Root task is running");

    while (mwifi_is_connected() && esp_mesh_is_root())
    {
        if (!mwifi_get_root_status())
        {
            vTaskDelay(500 / portTICK_RATE_MS);
            continue;
        }
        if (data_type.upgrade)
        { // This mesh package contains upgrade data.
            ret = mupgrade_root_handle(src_addr, data, size);
            MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mupgrade_root_handle", mdf_err_to_name(ret));
        }
        /**
         * @brief Recv data from node, and forward to mqtt server.
         */
        ret = mwifi_root_read(src_addr, &data_type, &data, &size, portMAX_DELAY);
        MDF_ERROR_GOTO(ret != MDF_OK, MEM_FREE, "<%s> mwifi_root_read", mdf_err_to_name(ret));

        ret = mesh_mqtt_write(src_addr, data, size, MESH_MQTT_DATA_JSON);

        MDF_ERROR_GOTO(ret != MDF_OK, MEM_FREE, "<%s> mesh_mqtt_publish", mdf_err_to_name(ret));

        /**
         * @brief Recv data from mqtt data queue, and forward to special device.
         */
        ret = mesh_mqtt_read(&request, pdMS_TO_TICKS(500));
        if (ret != MDF_OK)
        {
            MDF_FREE(request);
            continue;
        }
        else
        {
            // 如果收到的数据是json格式的数据，那么就解析出来
            cJSON *json = cJSON_Parse(request->data);
            char *firmware_name = NULL;
            if (json == NULL)
            {
                MDF_LOGE("JSON Parse Error format");
                continue;
            } // 如果消息是{"url":"http://wepper.club:8070/mupgrade.bin","version":"1.0.0"}，那么就解析出来
            cJSON *url = cJSON_GetObjectItem(json, "url");
            cJSON *version = cJSON_GetObjectItem(json, "version");
            if (url == NULL || version == NULL)
            {
                MDF_LOGE("JSON Parse Error format");
                continue;
            }
            // 将url-valuestring的值使用字符处理函数赋值给firmware_name
            firmware_name = (char *)malloc(strlen(url->valuestring) + 1);
            strcpy(firmware_name, url->valuestring);

            MDF_LOGI("url: %s, version: %s", firmware_name, version->valuestring);
            cJSON_Delete(json);
            xTaskCreate(ota_task, "ota_task", 8 * 1024,
                        firmware_name, CONFIG_MDF_TASK_DEFAULT_PRIOTY - 2, NULL);
        }
        if (ret != MDF_OK)
        {
            continue;
        }

        ret = mwifi_root_write(request->addrs_list, request->addrs_num, &data_type, request->data, request->size, true); // root节点向子节点发送数据
        MDF_ERROR_GOTO(ret != MDF_OK, MEM_FREE, "<%s> mwifi_root_write", mdf_err_to_name(ret));

    MEM_FREE:
        // MDF_FREE(root);
        MDF_FREE(request->addrs_list);
        MDF_FREE(request->data);
        MDF_FREE(request);
        // MDF_FREE(firmware_name);
    }

    MDF_LOGW("Root task is exit");

    MDF_FREE(data);
    mesh_mqtt_stop();
    vTaskDelete(NULL);
}

static void node_read_task(void *arg)
{
    mdf_err_t ret = MDF_OK;
    char *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    size_t size = MWIFI_PAYLOAD_LEN;
    mwifi_data_type_t data_type = {0x0};
    uint8_t src_addr[MWIFI_ADDR_LEN] = {0};

    MDF_LOGI("Node task is running");

    while (mwifi_is_connected())
    {
        size = MWIFI_PAYLOAD_LEN;
        memset(data, 0, MWIFI_PAYLOAD_LEN);
        ret = mwifi_read(src_addr, &data_type, data, &size, portMAX_DELAY);
        MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mwifi_root_recv", mdf_err_to_name(ret));

        if (data_type.upgrade)
        { // This mesh package contains upgrade data.
            ret = mupgrade_handle(src_addr, data, size);
            MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mupgrade_handle", mdf_err_to_name(ret));
        }
        else
        {
            MDF_LOGI("Receive [ROOT] addr: " MACSTR ", size: %d, data: %s",
                     MAC2STR(src_addr), size, data);

            /**
             * @brief Finally, the node receives a restart notification. Restart it yourself..
             */
            if (!strcmp(data, "restart"))
            {
                MDF_LOGI("Restart the version of the switching device");
                MDF_LOGW("The device will restart after 3 seconds");
                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_restart();
            }
        }
    }

    MDF_LOGW("Node read task is exit");

    MDF_FREE(data);
    vTaskDelete(NULL);
}

void node_write_task(void *arg)
{
    mdf_err_t ret = MDF_OK;
    size_t size = 0;
    char *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    mwifi_data_type_t data_type = {0x0};
    uint8_t sta_mac[MWIFI_ADDR_LEN] = {0};
    mesh_addr_t parent_mac = {0};

    MDF_LOGI("Node write task is running");
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);

    for (;;)
    {
        if (!mwifi_is_connected() || !mwifi_get_root_status())
        {
            vTaskDelay(500 / portTICK_RATE_MS);
            continue;
        }

        esp_mesh_get_parent_bssid(&parent_mac);
        size = asprintf(&data, "{\"type\":\"heartbeat\", \"self\": \"%02x%02x%02x%02x%02x%02x\", \"parent\":\"%02x%02x%02x%02x%02x%02x\",\"layer\":%d}",
                        MAC2STR(sta_mac), MAC2STR(parent_mac.addr), esp_mesh_get_layer());

        MDF_LOGD("Node send, size: %d, data: %s", size, data);
        ret = mwifi_write(NULL, &data_type, data, size, true);
        MDF_FREE(data);
        MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mwifi_write", mdf_err_to_name(ret));

        vTaskDelay(3000 / portTICK_RATE_MS);
    }

    MDF_LOGW("Node write task is exit");

    MDF_FREE(data);
    vTaskDelete(NULL);
}

/**
 * @brief All module events will be sent to this task in esp-mdf
 *
 * @Note:
 *     1. Do not block or lengthy operations in the callback function.
 *     2. Do not consume a lot of memory in the callback function.
 *        The task memory of the callback function is only 4KB.
 */
static mdf_err_t event_loop_cb(mdf_event_loop_t event, void *ctx)
{
    switch (event)
    {
    case MDF_EVENT_MWIFI_PARENT_CONNECTED:
        MDF_LOGI("Parent is connected on station interface");

        if (esp_mesh_is_root())
        {
            esp_netif_dhcpc_start(sta_netif);
        }
        xTaskCreate(node_read_task, "node_read_task", 4 * 1024,
                    NULL, CONFIG_MDF_TASK_DEFAULT_PRIOTY, NULL);

        break;
    case MDF_EVENT_MWIFI_PARENT_DISCONNECTED:
    {
        MDF_LOGI("Parent is disconnected on station interface");
        if (esp_mesh_is_root())
        {
            mesh_mqtt_stop();
        }
        break;
    }
    case MDF_EVENT_MWIFI_ROUTING_TABLE_ADD:
    case MDF_EVENT_MWIFI_ROUTING_TABLE_REMOVE:
    {
        MDF_LOGI("MDF_EVENT_MWIFI_ROUTING_TABLE_REMOVE, total_num: %d", esp_mesh_get_total_node_num());

        if (esp_mesh_is_root() && mwifi_get_root_status())
        {
            mdf_err_t err = mesh_mqtt_update_topo();

            if (err != MDF_OK)
            {
                MDF_LOGE("Update topo failed");
            }
        }
        break;
    }
    case MDF_EVENT_MWIFI_ROOT_GOT_IP: // 根节点获取到IP,也就是根节点连接到了路由器,则连接mqtt
        MDF_LOGI("Root obtains the IP address. It is posted by LwIP stack automatically");
        xTaskCreate(root_task, "root_read_task", 16 * 1024,
                    NULL, CONFIG_MDF_TASK_DEFAULT_PRIOTY, NULL);
        mesh_mqtt_start(MY_MQTT_URL);

        break;

    case MDF_EVENT_MUPGRADE_STARTED:
    {
        mupgrade_status_t status = {0x0};
        mupgrade_get_status(&status);

        MDF_LOGI("MDF_EVENT_MUPGRADE_STARTED, name: %s, size: %d",
                 status.name, status.total_size);
        break;
    }

    case MDF_EVENT_MUPGRADE_STATUS:
        MDF_LOGI("Upgrade progress: %d%%", (int)ctx);
        break;
    case MDF_EVENT_CUSTOM_MQTT_CONNECTED:
    {
        MDF_LOGI("MQTT connect");
        mdf_err_t err = mesh_mqtt_subscribe();
        if (err != MDF_OK)
        {
            MDF_LOGE("Subscribe failed");
        }
        err = mesh_mqtt_update_topo();
        if (err != MDF_OK)
        {
            MDF_LOGE("Update topo failed");
        }
        mwifi_post_root_status(true);
        break;
    }
    case MDF_EVENT_CUSTOM_MQTT_DISCONNECTED:
    {
        MDF_LOGI("MQTT disconnected");
        mwifi_post_root_status(false);
        break;
    }

    default:
        break;
    }

    return MDF_OK;
}

static mdf_err_t wifi_init()
{
    mdf_err_t ret = nvs_flash_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        MDF_ERROR_ASSERT(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    MDF_ERROR_ASSERT(ret);

    MDF_ERROR_ASSERT(esp_netif_init());
    MDF_ERROR_ASSERT(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&sta_netif, NULL));
    MDF_ERROR_ASSERT(esp_wifi_init(&cfg));
    MDF_ERROR_ASSERT(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    MDF_ERROR_ASSERT(esp_wifi_set_mode(WIFI_MODE_STA));
    MDF_ERROR_ASSERT(esp_wifi_set_ps(WIFI_PS_NONE));
    MDF_ERROR_ASSERT(esp_mesh_set_6m_rate(false));
    MDF_ERROR_ASSERT(esp_wifi_start());

    return MDF_OK;
}

void app_main()
{
    mwifi_init_config_t cfg = MWIFI_INIT_CONFIG_DEFAULT();
    mwifi_config_t config = {
        .channel = 0,
        .router_ssid = MY_ROUTER_SSID,
        .router_password = MY_ROUTER_PASSWORD,
        .mesh_id = MY_MESH_ID,
    };

    /**
     * @brief Set the log level for serial port printing.
     */
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_log_level_set("mupgrade_root", ESP_LOG_DEBUG);

    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    MDF_LOGI("Starting OTA example ...");

    /**
     * @brief Initialize wifi mesh.
     */
    MDF_ERROR_ASSERT(mdf_event_loop_init(event_loop_cb));
    MDF_ERROR_ASSERT(wifi_init());
    MDF_ERROR_ASSERT(mwifi_init(&cfg));
    MDF_ERROR_ASSERT(mwifi_set_config(&config));
    MDF_ERROR_ASSERT(mwifi_start());

    xTaskCreate(node_write_task, "node_write_task", 4 * 1024,
                NULL, CONFIG_MDF_TASK_DEFAULT_PRIOTY, NULL);

    // 新建读取dht11任务
    xTaskCreate(dht11_task, "dht11_task", 4 * 1024,
                NULL, CONFIG_MDF_TASK_DEFAULT_PRIOTY + 1, NULL);
}