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

uint8_t button_pressed = 0;

//button interrupt handler
static void btn_fxn(uint gpio, uint32_t eventMask) {
    static uint32_t last_ms = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - last_ms < 500) {
        return;
    }
    last_ms = now;

    if (gpio == SW1_PIN) {
        button_pressed = 1;
    } else if (gpio == SW2_PIN) {
        button_pressed = 1;
    }
}

//print task to handle button presses and play buzzer tones
static void print_task(void *arg) {
    (void)arg;
    while (1) {
        if (button_pressed) {
            printf("Button pressed!\n");
            buzzer_play_tone(1000, 200);
            button_pressed = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

//task to read IMU data and print it to the console
static void imuTask(void *pvParameters) {
    (void)pvParameters;
    float gx, gy, gz, ay, ax, az, t;

    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);
        stdio_flush();
        vTaskDelay(pdMS_TO_TICKS(500));

        //printf(">gx:%.2f,gy:%.2f,gz:%.2f\n", gx, gy, gz);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//task to read serial input and display it on the screen and play buzzer tones
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
            clear_display();
            write_text_xy(0, 0, message);
            
            for (int i = 0; i < sizeof(message) - 1; i++) {
                if (message[i] == '\0') {
                    break;
                }
                else if (message[i] == ' ') {
                    if (message[i + 1] == ' ') {
                        buzzer_play_tone(1000, 1400);
                        i++;
                    } else {
                        buzzer_play_tone(1000, 600);
                    }
                }
                else if (message[i] == '.') {
                    buzzer_play_tone(1000, 200);
                    sleep_ms(200);
                }
                else if (message[i] == '-') {
                    buzzer_play_tone(1000, 600);
                }
            }

            stdio_flush();

            index = 0;
        } else if (index < SERIAL_BUFFER_SIZE - 1) {
            message[index++] = (char)c;
        } else {
            index = 0;
        }
    }
}

// (en) Main function
int main() {
    stdio_init_all();
    init_hat_sdk();

    sleep_ms(300);

    // Näyttö: alustus ja teksti kerran
    init_display();
    clear_display();

    // Initialize the ICM42670 sensor
    init_ICM42670();
    ICM42670_startGyro(100, 2000);
    ICM42670_startAccel(100, 2);
    ICM42670_enable_accel_gyro_ln_mode();

    // init the buttons
    init_sw1();
    init_sw2();

    //init buzzer
    init_buzzer();

    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_FALL, true, btn_fxn);
    gpio_set_irq_enabled(SW2_PIN, GPIO_IRQ_EDGE_FALL, true);

    sleep_ms(50);

    xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(serialTask, "Serial Task", SERIAL_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(print_task, "Print Task", DEFAULT_STACK_SIZE, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);

    return 0;
}