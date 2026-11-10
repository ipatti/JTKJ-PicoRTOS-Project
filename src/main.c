
#include <stdio.h>
#include <string.h>

#include <pico/stdlib.h>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include "tkjhat/sdk.h"

#define IMU_TASK_STACK_SIZE 256


static void imuTask( void *pvParameters) {
    uint16_t gx, gy, gz;
    while (1) {
        ICM42670_read_sensor_data (float *ax, float *ay, float *az, float *gx, float *gy, float *gz, float *t)
        vTaskDelay(pdMS_TO_TICKS(10));
        printf("Gyro: gx=%d, gy=%d, gz=%d\n", gx, gy, gz);
    }
}


int main() {
    stdio_init_all();
    init_hat_sdk();

    sleep_ms(300);

    init_ICM42670();

    baseType_t result = xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);

    ICM42670_startGyro (uint16_t odr_hz, uint16_t fsr_dps)

    vTaskStartScheduler();

    return 0;
}