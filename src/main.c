#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>
#include <inttypes.h>
#include "tkjhat/sdk.h"
#include <semphr.h>

#define IMU_TASK_STACK_SIZE 1024
#define DEFAULT_STACK_SIZE 2048
#define SERIAL_TASK_STACK_SIZE 1024
#define SERIAL_BUFFER_SIZE 256

static SemaphoreHandle_t i2cMutex; 
uint8_t button1_pressed = 0;
uint8_t button2_pressed = 0;

char msg[SERIAL_BUFFER_SIZE];
size_t msg_index = 0;
bool z_positive = true;

//button interrupt handler
static void btn_fxn(uint gpio, uint32_t eventMask) {
    static uint32_t last_ms1 = 0;
    static uint32_t last_ms2 = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (gpio == SW1_PIN) {
        if (now - last_ms1 < 200) {
            return; // debounce
        }
        last_ms1 = now;
        button1_pressed = 1;
    } else if (gpio == SW2_PIN) {
        if (now - last_ms2 < 200) {
            return; // debounce
        }
        last_ms2 = now;
        button2_pressed = 1;
    }
}

//task to read IMU data and print it to the console
static void imuTask(void *pvParameters) {
    (void)pvParameters;
    float gx, gy, gz, ay, ax, az, t;

    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        xSemaphoreTake(i2cMutex, portMAX_DELAY);
        ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);
        xSemaphoreGive(i2cMutex);
        stdio_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
        if (z_positive && az < 0.0) {
            z_positive = false;

        } else if (!z_positive && az > 0.5) {
            z_positive = true;
        }
        //printf(">ax:%.2f,ay:%.2f,az:%.2f\n", ax, ay, az);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

//function to display text and play buzzer tones based on the input string
static void outputTask(const char *m) {

    uint8_t laskin = 0;
    uint8_t laskin2 = 0;
    char rivi[22];

    xSemaphoreTake(i2cMutex, portMAX_DELAY);

    clear_display();


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
    xSemaphoreGive(i2cMutex);

    if (msg_index == 0) {
    
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
}

static void inputTask(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        if (button1_pressed || button2_pressed) {
            vTaskDelay(pdMS_TO_TICKS(500)); // Wait for 500 ms to allow for both buttons to be pressed
            if (button1_pressed && !button2_pressed) {

                if (z_positive) {
                    if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                        msg[msg_index++] = '-';
                        msg[msg_index] = '\0'; // Null-terminate the string
                        outputTask(msg);
                    }
                } else {
                    if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                        msg[msg_index++] = '.';
                        msg[msg_index] = '\0'; // Null-terminate the string
                        outputTask(msg);
                    }
                }

                button1_pressed = 0;

            } else if (button2_pressed && !button1_pressed) {
                if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                    msg[msg_index++] = ' ';
                    msg[msg_index] = '\0'; // Null-terminate the string
                }
                button2_pressed = 0;
            }
            else if (button1_pressed && button2_pressed) {
                
                if (msg_index > 0) {               // don't send an empty message
                    printf("%s  \n", msg);
                    stdio_flush();
                    }
                vTaskDelay(pdMS_TO_TICKS(1000));
                outputTask(msg);
                msg_index = 0; // Reset the message index after outputting the message
                msg[0] = '\0'; // Clear the message buffer
                button1_pressed = 0;
                button2_pressed = 0;
                
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
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

    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_RISE, true, btn_fxn);
    gpio_set_irq_enabled(SW2_PIN, GPIO_IRQ_EDGE_RISE, true);

    i2cMutex = xSemaphoreCreateMutex();

    sleep_ms(50);

    xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(serialTask, "Serial Task", SERIAL_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(inputTask, "Input Task", DEFAULT_STACK_SIZE, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);

    return 0;
}