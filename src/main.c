#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include "tkjhat/sdk.h"

#define IMU_TASK_STACK_SIZE 1024
#define BUTTON_TASK_STACK_SIZE 256

static void buttonTask(void *pvParameters) {

    while(1) {
        if (gpio_get(SW1_PIN) == 1) {
            printf("SW1 pressed\n");
        }
        else if (gpio_get(SW2_PIN) == 1) {
            printf("SW2 pressed\n");
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
}


static void imuTask( void *pvParameters) {
    float gx, gy, gz;
    float ax, ay, az;
    float t;
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        ICM42670_read_sensor_data(&ax, &ay, &az,&gx, &gy, &gz, &t);

        vTaskDelay(pdMS_TO_TICKS(1000));

        printf("Gyro: gx=%.2f, gy=%.2f, gz=%.2f\n", gx, gy, gz);
    }
}


int main() {

    stdio_init_all();
    init_hat_sdk();

    sleep_ms(300);

    // Initialize the ICM42670 sensor
    init_ICM42670();
    ICM42670_startGyro(100, 2000);
    ICM42670_startAccel(100,2);
    ICM42670_enable_accel_gyro_ln_mode();

    //init the buttons
    init_sw1();
    init_sw2();

    sleep_ms(50);

    //Create imutask
    BaseType_t result = xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);

    //Create button task
    BaseType_t buttonResult = xTaskCreate(buttonTask, "Button Task", BUTTON_TASK_STACK_SIZE, NULL, 1, NULL);

    vTaskStartScheduler();

    return 0;
}