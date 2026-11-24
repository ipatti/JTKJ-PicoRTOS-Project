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
#define MORSE_TIME_UNIT_MS 200
#define IMU_READ_PERIOD_MS 100

typedef enum {
    STATE_WAITING = 1,
    STATE_INPUT,
    STATE_SENDING,
    STATE_PLAYING
} state_t;

volatile state_t programState = STATE_WAITING;

// display and IMU share the same I2C bus -> only one task may use it at a time
static SemaphoreHandle_t i2cMutex;
// buzzer is shared too: received messages and typed symbols must not play at the same time
static SemaphoreHandle_t buzzerMutex;

volatile uint8_t button1_pressed = 0;
volatile uint8_t button2_pressed = 0;

char msg[SERIAL_BUFFER_SIZE];
size_t msg_index = 0;
volatile bool z_positive = true;

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
    float gx = 0, gy = 0, gz = 0, ay = 0, ax = 0, az = 0, t = 0;

    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(IMU_READ_PERIOD_MS));

        if (programState != STATE_WAITING) {
            continue;
        }

        // read first, then use the value right away (it's fresh, not 500 ms old)
        xSemaphoreTake(i2cMutex, portMAX_DELAY);
        int rc = ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);
        xSemaphoreGive(i2cMutex);

        if (rc != 0) {
            continue;   // failed read: keep the previous z_positive
        }

        // hysteresis: flip only clearly past the thresholds, so it doesn't flicker near flat
        if (z_positive && az < 0.0) {
            z_positive = false;
        } else if (!z_positive && az > 0.5) {
            z_positive = true;
        }
        //printf(">ax:%.2f,ay:%.2f,az:%.2f\n", ax, ay, az);
    }
}

//plays one Morse symbol ('.' or '-') on the buzzer, followed by the 1-unit gap between symbols
static void playSymbol(char c) {
    if (c == '.') {
        buzzer_play_tone(1000, MORSE_TIME_UNIT_MS);
        vTaskDelay(pdMS_TO_TICKS(MORSE_TIME_UNIT_MS));
    } else if (c == '-') {
        buzzer_play_tone(1000, 3 * MORSE_TIME_UNIT_MS);
        vTaskDelay(pdMS_TO_TICKS(MORSE_TIME_UNIT_MS));
    }
}

//function to display text and (if play_sound) play the whole message on the buzzer
static void outputTask(const char *m, bool play_sound) {

    uint8_t laskin = 0;
    uint8_t laskin2 = 0;
    char rivi[22];

    // Display and IMU share the I2C bus -> lock it while drawing.
    // Note: the SDK's write_text_xy() sleeps 800 ms per call, so the lock is held ~0.8 s per row.
    xSemaphoreTake(i2cMutex, portMAX_DELAY);

    clear_display();

    for (int i = 0; m[i] != '\0' && laskin2 < 8; i++) {   // display has 8 rows
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

    // Buzzer playback: no I2C, so outside the I2C lock. vTaskDelay lets other tasks run during the gaps.
    // buzzerMutex: only one task may drive the buzzer at a time (tasks run on two cores).
    if (play_sound) {
        xSemaphoreTake(buzzerMutex, portMAX_DELAY);

        for (int i = 0; m[i] != '\0'; i++) {

            if (m[i] == ' ') {
                if (m[i + 1] == ' ') {
                    vTaskDelay(pdMS_TO_TICKS(6 * MORSE_TIME_UNIT_MS));   // word gap (7 units incl. symbol gap)
                    i++;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(2 * MORSE_TIME_UNIT_MS));   // letter gap (3 units incl. symbol gap)
                }
            } else {
                playSymbol(m[i]);
            }
        }

        xSemaphoreGive(buzzerMutex);
    }
}

//feedback beep for a symbol the user just typed. If the buzzer is busy playing a message
//from the PC, skip the beep instead of waiting (typing must not freeze for seconds).
static void beepTypedSymbol(char c) {
    if (xSemaphoreTake(buzzerMutex, 0) == pdTRUE) {
        playSymbol(c);
        xSemaphoreGive(buzzerMutex);
    }
}

static void inputTask(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        if (programState == STATE_WAITING && (button1_pressed || button2_pressed)) {
            programState = STATE_INPUT;
            vTaskDelay(pdMS_TO_TICKS(500)); // Wait for 500 ms to allow for both buttons to be pressed

            if (button1_pressed && !button2_pressed) {

                if (z_positive) {
                    if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                        msg[msg_index++] = '-';
                        msg[msg_index] = '\0'; // Null-terminate the string
                        outputTask(msg, false);  // show it, don't replay the whole message
                        beepTypedSymbol('-');
                    }
                } else {
                    if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                        msg[msg_index++] = '.';
                        msg[msg_index] = '\0'; // Null-terminate the string
                        outputTask(msg, false);  // show it, don't replay the whole message
                        beepTypedSymbol('.');
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
                programState = STATE_SENDING;

                if (msg_index > 0) {               // don't send an empty message
                    printf("%s  \n", msg);
                }
                outputTask(msg, false);
                msg_index = 0; // Reset the message index after outputting the message
                msg[0] = '\0'; // Clear the message buffer
                button1_pressed = 0;
                button2_pressed = 0;
            }

            programState = STATE_WAITING;
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
            while (programState != STATE_WAITING) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            programState = STATE_PLAYING;

            message[index] = '\0';

            printf("%s  \n", message);

            outputTask(message, true);   // messages from the PC are always played

            stdio_flush();

            index = 0;
            programState = STATE_WAITING;
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


    sleep_ms(50);

    i2cMutex = xSemaphoreCreateMutex();
    buzzerMutex = xSemaphoreCreateMutex();


    xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(serialTask, "Serial Task", SERIAL_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(inputTask, "Input Task", DEFAULT_STACK_SIZE, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);

    return 0;
}