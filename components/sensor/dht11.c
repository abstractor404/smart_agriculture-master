#include "dht11.h"
#include "esp_task_wdt.h"
#define TAG "DHT11"

#define UPLOAD_INTERVAL_MIN 5000  // 最小上传间隔，单位为毫秒
#define UPLOAD_INTERVAL_MAX 30000 // 最大上传间隔，单位为毫秒
#define TEMP_HUMI_PIN DHT11_PIN   // 温湿度传感器引脚
#define CHANGE_THRESHOLD 0.3      // 温湿度变化的阈值

static void InputInitial(void) // 设置端口为输入
{
    esp_rom_gpio_pad_select_gpio(DHT11_PIN);
    gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);
}

static void OutputHigh(void) // 输出1
{
    esp_rom_gpio_pad_select_gpio(DHT11_PIN);
    gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_PIN, 1);
}

static void OutputLow(void) // 输出0
{
    esp_rom_gpio_pad_select_gpio(DHT11_PIN);
    gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_PIN, 0);
}

typedef struct
{
    uint8_t humi_int;  // 湿度的整数部分
    uint8_t humi_deci; // 湿度的小数部分
    uint8_t temp_int;  // 温度的整数部分
    uint8_t temp_deci; // 温度的小数部分
    uint8_t check_sum; // 校验和
} DHT11_Data_TypeDef;

DHT11_Data_TypeDef DHT11_Data;

/*
 * 从DHT11读取一个字节，MSB先行
 */
static uint8_t Read_Byte(void)
{
    uint8_t i, temp = 0;

    for (i = 0; i < 8; i++)
    {
        /*每bit以50us低电平标置开始，轮询直到从机发出 的50us 低电平 结束*/
        while (gpio_get_level(DHT11_PIN) == 0)
        {
            // esp_rom_delay_us(1); // 延时1us防止死循环
        }

        /*DHT11 以26~28us的高电平表示“0”，以70us高电平表示“1”，
         *通过检测 x us后的电平即可区别这两个状 ，x 即下面的延时
         */
        esp_rom_delay_us(40); // 延时x us 这个延时需要大于数据0持续的时间即可

        if (gpio_get_level(DHT11_PIN) == 1) /* x us后仍为高电平表示数据“1” */
        {
            /* 等待数据1的高电平结束 */
            while (gpio_get_level(DHT11_PIN) == 1)
            {
                esp_rom_delay_us(1); // 延时1us
            }

            temp |= (uint8_t)(0x01 << (7 - i)); // 把第7-i位置1，MSB先行
        }
        else // x us后为低电平表示数据“0”
        {
            temp &= (uint8_t) ~(0x01 << (7 - i)); // 把第7-i位置0，MSB先行
        }
    }
    return temp;
}
/*
 * 一次完整的数据传输为40bit，高位先出
 * 8bit 湿度整数 + 8bit 湿度小数 + 8bit 温度整数 + 8bit 温度小数 + 8bit 校验和
 */
static uint8_t Read_DHT11(DHT11_Data_TypeDef *DHT11_Data)
{
    /*输出模式*/
    /*主机拉低*/
    OutputLow();
    /*延时18ms*/
    vTaskDelay(18);

    /*总线拉高 主机延时30us*/
    OutputHigh();
    esp_rom_delay_us(20); // 延时30us

    /*主机设为输入 判断从机响应信号*/
    InputInitial();

    /*判断从机是否有低电平响应信号 如不响应则跳出，响应则向下运行*/
    if (gpio_get_level(DHT11_PIN) == 0)
    {
        // 禁止任务调度
        vTaskSuspendAll();
        /*轮询直到从机发出 的80us 低电平 响应信号结束*/
        while (gpio_get_level(DHT11_PIN) == 0)
        {
            // esp_rom_delay_us(1); // 延时1us
        }

        /*轮询直到从机发出的 80us 高电平 标置信号结束*/
        while (gpio_get_level(DHT11_PIN) == 1)
        {
            // esp_rom_delay_us(1); // 延时30us
        }
        /*开始接收数据*/
        DHT11_Data->humi_int = Read_Byte();

        DHT11_Data->humi_deci = Read_Byte();

        DHT11_Data->temp_int = Read_Byte();

        DHT11_Data->temp_deci = Read_Byte();

        DHT11_Data->check_sum = Read_Byte();
        xTaskResumeAll();
        /*读取结束，引脚改为输出模式*/
        /*主机拉高*/
        OutputHigh();

        /*检查读取的数据是否正确*/
        if (DHT11_Data->check_sum == DHT11_Data->humi_int + DHT11_Data->humi_deci + DHT11_Data->temp_int + DHT11_Data->temp_deci)
            return 1;
        else
            return 0;
    }
    else
    {
        return 0;
    }
}

// 创建读取温湿度的任务
static void dht11_init(void)
{
    // 初始化SOIL_PIN为输入
    gpio_config_t io_conf;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << SOIL_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    // 初始化继电器引脚
    gpio_config_t io_conf1;
    io_conf1.mode = GPIO_MODE_OUTPUT;
    io_conf1.pin_bit_mask = (1ULL << RELAY_PIN);
    io_conf1.pull_down_en = 1;
    gpio_config(&io_conf1);

    // 初始化sensor_light引脚为模拟量输入
    adc1_config_width(ADC_WIDTH_BIT_13);
    adc1_config_channel_atten(ADC2_CHANNEL_3, ADC_ATTEN_DB_11);

    // while (1)
    // {
    //     if (Read_DHT11(&DHT11_Data))
    //     {
    //         sprintf(dht11_buff, "Temp=%.2f--Humi=%.2f%%RH \r\n", DHT11_Data.temp_int + DHT11_Data.temp_deci / 10.0, DHT11_Data.humi_int + DHT11_Data.humi_deci / 10.0);
    //         printf("%s", dht11_buff);
    //     }
    //     else
    //     {
    //         printf("DHT11 Read Error!\r\n");
    //     }
    //     vTaskDelay(100);
    //     // 读取土壤湿度低电平为湿
    //     soil_humidity = gpio_get_level(SOIL_PIN);
    //     if (soil_humidity == 0)
    //     {
    //         // printf("shi\n");
    //         //继电器写入低电平
    //         gpio_set_level(RELAY_PIN, 1);
    //     }
    //     else
    //     {
    //         // printf("gan\n");
    //         //继电器写入高电平
    //         gpio_set_level(RELAY_PIN, 0);
    //     }
    // }
}

void dht11_task(void *pvParameters)
{
    char *dht11_buff = NULL;

    TickType_t lastUploadTime = 0;
    uint16_t uploadInterval = UPLOAD_INTERVAL_MIN / portTICK_PERIOD_MS; // 初始上传间隔，单位为系统时钟周期

    DHT11_Data_TypeDef prevDhtData; // 上一次的温湿度数据
    prevDhtData.humi_int = 0;
    prevDhtData.humi_deci = 0;
    prevDhtData.temp_int = 0;
    prevDhtData.temp_deci = 0;
    // 定义sensir_light的值
    uint16_t sensor_light = 0;
    mwifi_data_type_t data_type = {0x0};
    mdf_err_t ret = MDF_OK;
    size_t size = 0;

    esp_task_wdt_delete(NULL);
    dht11_init();
    while (1)
    {
        if (xTaskGetTickCount() - lastUploadTime >= uploadInterval)
        {
            DHT11_Data_TypeDef dhtData; // 温湿度数据

            lastUploadTime = xTaskGetTickCount(); // 更新上次上传时间
            if (Read_DHT11(&dhtData))
            {
                // 执行上传温湿度的操作
                // 数据转为json格式放在dht11_buff中
                sensor_light = adc1_get_raw(ADC2_CHANNEL_3);
                size = asprintf(&dht11_buff, "{\"version\":\"%s\",\"Temp\":\"%.2f\",\"Humi\":\"%.2f\",\"sensor_light\":\"%d\"}", VERSION, dhtData.temp_int + dhtData.temp_deci / 10.0, dhtData.humi_int + dhtData.humi_deci / 10.0, sensor_light);
                ret = mwifi_write(NULL, &data_type, dht11_buff, size, true);
                MDF_LOGD("Node send, size: %d, data: %s", size, dht11_buff);
                MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mwifi_write", mdf_err_to_name(ret));
                // 读取sensor_light引脚的模拟量

                printf("sensor_light = %d\n", sensor_light);
                printf("Temp=%.2f--Humi=%.2f%%RH \r\n", dhtData.temp_int + dhtData.temp_deci / 10.0, dhtData.humi_int + dhtData.humi_deci / 10.0);
                free(dht11_buff);
            }
            else
            {
                printf("DHT11 Read Error!\r\n");
            }

            // 根据当前温湿度数据，调整上传间隔
            if (abs(dhtData.humi_int - prevDhtData.humi_int) >= CHANGE_THRESHOLD ||
                abs(dhtData.temp_int - prevDhtData.temp_int) >= CHANGE_THRESHOLD)
            {
                uploadInterval = UPLOAD_INTERVAL_MIN / portTICK_PERIOD_MS;
            }
            else
            {
                uploadInterval = UPLOAD_INTERVAL_MAX / portTICK_PERIOD_MS;
            }
            if (dhtData.humi_int > 80)
            {
                uploadInterval += 100; // 增加100个系统时钟周期
                if (uploadInterval > UPLOAD_INTERVAL_MAX / portTICK_PERIOD_MS)
                {
                    uploadInterval = UPLOAD_INTERVAL_MAX / portTICK_PERIOD_MS;
                }
            }
            else if (dhtData.humi_int < 60)
            {
                uploadInterval -= 100; // 减小100个系统时钟周期
                if (uploadInterval < UPLOAD_INTERVAL_MIN / portTICK_PERIOD_MS)
                {
                    uploadInterval = UPLOAD_INTERVAL_MIN / portTICK_PERIOD_MS;
                }
            }

            prevDhtData = dhtData; // 更新上一次的温湿度数据
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS); // 任务延时，单位为系统时钟周期
    }
}
