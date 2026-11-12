#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include "tkjhat/sdk.h"

#define IMU_TASK_STACK_SIZE 1024
#define DEFAULT_STACK_SIZE 2048
#define SERIAL_TASK_STACK_SIZE 1024
#define SERIAL_BUFFER_SIZE 256


static void btn_fxn(uint gpio, uint32_t eventMask) {
    if (gpio == SW1_PIN) {
        printf("button1\n");
    } else if (gpio == SW2_PIN) {
        printf("button2\n");
    }
}


static void imuTask(void *pvParameters) {
    (void)pvParameters;
    float gx, gy, gz, ay, ax, az, t;
    
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);
        //flushesh the buffer :)
        stdio_flush();
        vTaskDelay(pdMS_TO_TICKS(500));

        printf(">gx=%.2f,gy=%.2f,gz=%.2f\n", gx, gy, gz);
        
    }
}

static void serialTask(void *pvParameters) {
    (void)pvParameters;

    char message[SERIAL_BUFFER_SIZE];
    size_t index = 0;

    while (1) {
        int c = getchar_timeout_us(0);

        if (c == PICO_ERROR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            message[index] = '\0';

            printf("%s  \n", message);
            stdio_flush();

            index = 0;
        } else if (index < SERIAL_BUFFER_SIZE - 1) {
            message[index++] = (char)c;
        } else {
            index = 0;
        }
    }
}



int main() {
    stdio_init_all();
    init_hat_sdk();

    sleep_ms(300);

    // Initialize the ICM42670 sensor
    init_ICM42670();
    ICM42670_startGyro(100, 2000);
    ICM42670_startAccel(100, 2);
    ICM42670_enable_accel_gyro_ln_mode();

    // init the buttons
    init_sw1();
    init_sw2();

    // Rekisteröidään keskeytykset napeille SW1_PIN ja SW2_PIN
    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_FALL, true, btn_fxn);
    gpio_set_irq_enabled(SW2_PIN, GPIO_IRQ_EDGE_FALL, true);

    sleep_ms(50);

    // Create imutask
    BaseType_t result = xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);
    BaseType_t serialResult = xTaskCreate(serialTask, "Serial Task", SERIAL_TASK_STACK_SIZE, NULL, 1, NULL);


    vTaskStartScheduler();

    while (1);
    return 0;
}