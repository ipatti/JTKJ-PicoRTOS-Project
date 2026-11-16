#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>
#include <inttypes.h>
#include <inttypes.h>

#include "tkjhat/sdk.h"

#define IMU_TASK_STACK_SIZE 1024
#define DEFAULT_STACK_SIZE 2048
#define SERIAL_TASK_STACK_SIZE 1024
#define SERIAL_BUFFER_SIZE 256
#define MORSE_TIME_UNIT_MS 200
#define MESSAGE_QUEUE_LENGTH 5

uint8_t button_pressed = 0;
static QueueHandle_t msgQueue;

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
            buzzer_play_tone(1000, MORSE_TIME_UNIT_MS);
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


//function to display text and play buzzer tones based on the input string
static void outputTask(const char *m) {

    uint8_t laskin = 0;
    uint8_t laskin2 = 0;
    char rivi[22];

    for (int i = 0; m[i] != '\0'; i++) {
        rivi[laskin] = m[i];
        laskin++;

        // If the line is full or it's the end of the message, print the line and reset the counter
        if (laskin == 21 || m[i + 1] == '\0') {
            rivi[laskin] = '\0';
            write_text_xy(0, laskin2 * 8, rivi);
            laskin2++;
            laskin = 0;
        }
    }

    for (int i = 0; m[i] != '\0'; i++) {
                
        if (m[i] == ' ') {
            if (m[i + 1] == ' ') {
                sleep_ms(1400);
                i++;
            } else {
                sleep_ms(600);
            }
        }
        else if (m[i] == '.') {
            buzzer_play_tone(1000, 200);
            sleep_ms(200);
        }
        else if (m[i] == '-') {
            buzzer_play_tone(1000, 600);
        }
    }
}


//task to take messages from queue and output them
static void queueTask(void *pvParameters) {
    (void)pvParameters;
    char m[SERIAL_BUFFER_SIZE];

    while (1) {
        xQueueReceive(msgQueue, m, portMAX_DELAY);

        printf("%s  \n", m);
        clear_display();
        outputTask(m);
        stdio_flush();
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

            if (xQueueSend(msgQueue, message, 0) != pdPASS) {
                printf("Queue full, message dropped\n");
            }

            printf("%s  \n", message);
            clear_display();

            outputTask(message);

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

    // queue for "buffering" the messages send from serial client. Prevents the loss of messages before the device can output them
    msgQueue = xQueueCreate(MESSAGE_QUEUE_LENGTH, SERIAL_BUFFER_SIZE);

    xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(serialTask, "Serial Task", SERIAL_TASK_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(print_task, "Print Task", DEFAULT_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(queueTask, "Queue Task", DEFAULT_STACK_SIZE, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);

    return 0;
}