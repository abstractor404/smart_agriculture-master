#ifndef _DHT11_H_
#define _DHT11_H_

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
//包含adc的头文件
#include "driver/adc.h"
#include "mwifi.h"

#include "math.h"

#define DHT11_PIN 26        // 定义DHT11的引脚
#define SENSOR_LIGHT_PIN 14 // 定义光敏传感器的引脚
#define SOIL_PIN 17         // 定义土壤湿度传感器的引脚
#define RELAY_PIN 20 // 继电器引脚

#define VERSION "1.0.0"// 版本号
void dht11_task(void *pvParameters);

#endif