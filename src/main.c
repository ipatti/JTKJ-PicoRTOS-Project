// Joona Saarela & Aleksi Pyyny

// AI use (Claude Opus 5.5):
// -Mutex fix: We had a bug that caused a freeze on boot. After long debugging session, Opus 5.5 was able to find that creating mutexes on main() after sleep_ms(50) fixes the bug.
// -Opus also created the buzzerMutex
// -Functions Opus 5.5 created; beepTypedSymbol(), buzzChord(), buzzNote(), playTyhmaSnippet()
// -Opus 5.5 was also used to find some small bugs/edgecases that might have caused problems ()


#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <pico/stdlib.h>
#include <pico/time.h>

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>

#include "tkjhat/sdk.h"


//configuration
#define IMU_TASK_STACK_SIZE 1024
#define DEFAULT_STACK_SIZE 2048
#define SERIAL_TASK_STACK_SIZE 1024
#define SERIAL_BUFFER_SIZE 256
#define MORSE_TIME_UNIT_MS 200
#define IMU_READ_PERIOD_MS 100

// How fast the two chord notes alternate (ms per note). Smaller = smoother, more "chord"-like;
// larger = audible bouncing/trill. Useful range ~8-40 ms.
// Created by Opus when creating the buzzer music related functions
#define BUZZ_ARP_MS 20


// State and shared variables
typedef enum {
    STATE_WAITING = 1,
    STATE_INPUT,
    STATE_SENDING,
    STATE_PLAYING
} state_t;

volatile state_t programState = STATE_WAITING;

// Display and IMU share the same I2C bus -> only one task may use it at a time
static SemaphoreHandle_t i2cMutex;

// Buzzer mutex to prevent received messages and typed symbols playing at the same time
// Created by opus5.5
static SemaphoreHandle_t buzzerMutex;

volatile uint8_t button1_pressed = 0;
volatile uint8_t button2_pressed = 0;

char msg[SERIAL_BUFFER_SIZE];
size_t msg_index = 0;
volatile bool z_positive = true;



// Button interrupt handler (200 ms debounce per button)
//Joona
static void btn_fxn(uint gpio, uint32_t eventMask) {
    static uint32_t last_ms1 = 0;
    static uint32_t last_ms2 = 0;

    //button debounce: ignore presses that are too close together
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (gpio == SW1_PIN) {
        if (now - last_ms1 < 200) {
            return;
        }
        last_ms1 = now;
        button1_pressed = 1;

    } else if (gpio == SW2_PIN) {
        if (now - last_ms2 < 200) {
            return;
        }
        last_ms2 = now;
        button2_pressed = 1;
    }
}


// Reads the IMU and keeps z_positive up to date (decides between '.' and '-')
//Joona and Aleksi
static void imuTask(void *pvParameters) {
    (void)pvParameters;

    float gx = 0, gy = 0, gz = 0, ay = 0, ax = 0, az = 0, t = 0;

    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(IMU_READ_PERIOD_MS));

        // If we're not in the waiting state, don't read the IMU
        // this check was added by opus5.5
        if (programState != STATE_WAITING) {
            continue;
        }


        xSemaphoreTake(i2cMutex, portMAX_DELAY);
        int rc = ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);
        xSemaphoreGive(i2cMutex);

        // this check was also added by opus5.5 
        if (rc != 0) {
            continue;   // Failed read -> keep the previous z_positive
        }

        // Flip z_positive based imu readings (az)
        if (z_positive && az < 0.0) {
            z_positive = false;
        } else if (!z_positive && az > 0.0) {
            z_positive = true;
        }
    }
}


// Play either "." or "-" on buzzer
//Aleksi
static void playSymbol(char c) {
    if (c == '.') {
        buzzer_play_tone(1000, MORSE_TIME_UNIT_MS);
        vTaskDelay(pdMS_TO_TICKS(MORSE_TIME_UNIT_MS));
    } else if (c == '-') {
        buzzer_play_tone(1000, 3 * MORSE_TIME_UNIT_MS);
        vTaskDelay(pdMS_TO_TICKS(MORSE_TIME_UNIT_MS));
    }
}

// Shows the text on the display and (if play_sound) plays the whole message on the buzzer
//Joona And Aleksi
static void outputTask(const char *m, bool play_sound) {
    uint8_t col = 0;
    uint8_t row = 0;
    char rivi[22];

    // Display and IMU share the i2c bus -> lock it while drawing.
    xSemaphoreTake(i2cMutex, portMAX_DELAY);

    clear_display();

    // Display the message, 21 characters per line, 8 lines max
    for (int i = 0; m[i] != '\0' && row < 8; i++) {   // Display has 8 rows
        rivi[col] = m[i];
        col++;

        // Line full or end of message
        if (col == 21 || m[i + 1] == '\0') {
            rivi[col] = '\0';
            write_text_xy(0, row * 8, rivi);
            row++;
            col = 0;
        }
    }

    xSemaphoreGive(i2cMutex);


    // This part is created by opus5.5
    // Buzzer playback: no I2C, so outside the I2C lock. vTaskDelay lets other tasks run during the gaps.
    // buzzerMutex: only one task may drive the buzzer at a time.
    if (play_sound) {
        xSemaphoreTake(buzzerMutex, portMAX_DELAY);

        for (int i = 0; m[i] != '\0'; i++) {
            if (m[i] == ' ') {
                if (m[i + 1] == ' ') {
                    vTaskDelay(pdMS_TO_TICKS(6 * MORSE_TIME_UNIT_MS));   // Word gap (7 units incl. symbol gap)
                    i++;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(2 * MORSE_TIME_UNIT_MS));   // Letter gap (3 units incl. symbol gap)
                }
            } else {
                playSymbol(m[i]);
            }
        }

        xSemaphoreGive(buzzerMutex);
    }
}

// Feedback beep for a symbol the user just typed. If the buzzer is busy playing a message
// from the PC, skip the beep instead of waiting.
// Created by opus5.5

static void beepTypedSymbol(char c) {
    if (xSemaphoreTake(buzzerMutex, 0) == pdTRUE) {
        playSymbol(c);
        xSemaphoreGive(buzzerMutex);
    }
}

// Buzzer music
// Two notes "at once" on a one-tone buzzer: switch between them every BUZZ_ARP_MS (fast arpeggio),
// then a 15 ms gap so repeated notes stay separate
// Note: helper function created by Claude Opus 5.5
//Aleksi
static void buzzChord(uint16_t lo, uint16_t hi, uint16_t ms) {
    uint16_t play = ms - 15;

    for (uint16_t t = 0; t < play; t += BUZZ_ARP_MS) {
        uint16_t d = (play - t < BUZZ_ARP_MS) ? (play - t) : BUZZ_ARP_MS;
        buzzer_play_tone(((t / BUZZ_ARP_MS) & 1) ? hi : lo, d);
    }

    vTaskDelay(pdMS_TO_TICKS(15));
}

// Single note + 15 ms gap
// Note: helper function created by Claude Opus 5.5
//Aleksi
static void buzzNote(uint16_t f, uint16_t ms) {
    buzzer_play_tone(f, ms - 15);
    vTaskDelay(pdMS_TO_TICKS(15));
}

// Draws a smiley on the display ("message sent" picture)
//NOTE: This function was created by gemini hydrogen
//Prompt: "Draw a smiley face on the display using the draw_circle and draw_line functions.
//The smiley should have a circular face, two eyes, and a smiling mouth.
//Joona
static void drawSmiley(void) {
    xSemaphoreTake(i2cMutex, portMAX_DELAY);

    clear_display();

    // Face
    draw_circle(64, 32, 25, false);

    // Eyes
    draw_circle(55, 25, 3, true);
    draw_circle(73, 25, 3, true);

    // Smile
    draw_line(48, 38, 52, 44);
    draw_line(52, 44, 58, 48);
    draw_line(58, 48, 64, 50);
    draw_line(64, 50, 70, 48);
    draw_line(70, 48, 76, 44);
    draw_line(76, 44, 80, 38);

    xSemaphoreGive(i2cMutex);
}

// Plays the "Tyhmä" 3xOSC snippet (2 bars, ~102 BPM, ~4.4 s), one octave above the piano roll.
// E = 1/8 note (two piano-roll steps), S = 1/32 note (the short F notes).
// Hz: F5 698, F#5 740, G#5 831, A#5 932, C6 1047, C#6 1109, D#6 1245, F6 1397
// Note: function created by Claude Opus 5.5
// I used a screenshot of a piano roll sketch I made, and prompted it to create a function that plays that specific melody via buzzer. Opus 5.5 created this function, buzzChord() and buzzNote()
//Aleksi
static void playTyhmaSnippet(void) {
    const uint16_t E = 294;   // 1/8 note at 102 BPM
    const uint16_t S = 73;    // 1/32 note
    xSemaphoreTake(buzzerMutex, portMAX_DELAY);   // Nothing else may beep meanwhile

    // ---- Bar 1 ----
    buzzChord(932, 1109, E);                                // A#5 + C#6
    buzzNote(698, S);  vTaskDelay(pdMS_TO_TICKS(3 * S));    // F5 short, then rest until step 4
    buzzChord(698, 1109, E);                                // F5 + C#6
    vTaskDelay(pdMS_TO_TICKS(E));                           // Rest
    buzzChord(698, 1397, E);                                // F5 + F6
    buzzChord(740, 1245, E);                                // F#5 + D#6
    buzzChord(698, 1109, E);                                // F5 + C#6
    vTaskDelay(pdMS_TO_TICKS(E));                           // Rest

    // ---- Bar 2 ----
    buzzChord(698, 1109, E);                                // F5 + C#6
    buzzNote(698, S);  vTaskDelay(pdMS_TO_TICKS(3 * S));    // F5 short, then rest until step 4
    buzzChord(698, 1047, E);                                // F5 + C6
    vTaskDelay(pdMS_TO_TICKS(E));                           // Rest
    buzzChord(831, 1109, E);                                // G#5 + C#6
    buzzChord(932, 1245, E);                                // A#5 + D#6
    buzzChord(698, 1109, E);                                // F5 + C#6

    xSemaphoreGive(buzzerMutex);
}



// Button 1 adds '.' or '-', button 2 adds a space, both buttons pressed at same time sends the message
//Joona
static void inputTask(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        // If we're not in the waiting state, don't process input
        if (programState == STATE_WAITING && (button1_pressed || button2_pressed)) {

            //Go to input state, wait
            programState = STATE_INPUT;
            vTaskDelay(pdMS_TO_TICKS(500));   // Wait 500 ms to allow for both buttons to be pressed

            if (button1_pressed && !button2_pressed) {
                // Button 1: add '-' or '.'
                if (z_positive) {
                    if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                        msg[msg_index++] = '-';
                        msg[msg_index] = '\0';
                        outputTask(msg, false);
                        beepTypedSymbol('-');
                    }
                } else {
                    if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                        msg[msg_index++] = '.';
                        msg[msg_index] = '\0';
                        outputTask(msg, false);
                        beepTypedSymbol('.');
                    }
                }

                button1_pressed = 0;
            } else if (button2_pressed && !button1_pressed) {
                // Button 2: add a space
                if (msg_index < SERIAL_BUFFER_SIZE - 1) {
                    msg[msg_index++] = ' ';
                    msg[msg_index] = '\0';
                }

                button2_pressed = 0;
            } else if (button1_pressed && button2_pressed) {
                // Both buttons: send the message to the PC and change state to STATE_SENDING. If the message is empty, don't send it.
                programState = STATE_SENDING;

                bool sent = false;
                if (msg_index > 0) {   // Don't send an empty message
                    printf("%s  \n", msg);
                    sent = true;
                }

                outputTask(msg, false);
                if (sent) {
                    // "Message sent" feedback (~4.4 s), still in STATE_SENDING
                    drawSmiley();
                    playTyhmaSnippet();

                    xSemaphoreTake(i2cMutex, portMAX_DELAY);   // Display uses I2C -> lock
                    clear_display();
                    xSemaphoreGive(i2cMutex);
                }

                msg_index = 0;    // Reset the message
                msg[0] = '\0';
                button1_pressed = 0;
                button2_pressed = 0;
            }
            //switch back to waiting state
            programState = STATE_WAITING;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


// Reads lines from the PC, then shows and plays them
//Aleksi
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

            outputTask(message, true);   // Messages from the PC are always played
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


int main() {
    stdio_init_all();
    init_hat_sdk();
    sleep_ms(300);

    // Display
    init_display();
    clear_display();

    // IMU (ICM42670)
    init_ICM42670();
    ICM42670_startGyro(100, 2000);
    ICM42670_startAccel(100, 2);
    ICM42670_enable_accel_gyro_ln_mode();

    // Buttons and buzzer
    init_sw1();
    init_sw2();
    init_buzzer();

    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_RISE, true, btn_fxn);
    gpio_set_irq_enabled(SW2_PIN, GPIO_IRQ_EDGE_RISE, true);

    sleep_ms(50);

    i2cMutex = xSemaphoreCreateMutex();
    buzzerMutex = xSemaphoreCreateMutex();

    // Tasks
    xTaskCreate(imuTask,    "IMU Task",    IMU_TASK_STACK_SIZE,    NULL, 1, NULL);
    xTaskCreate(serialTask, "Serial Task", SERIAL_TASK_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(inputTask,  "Input Task",  DEFAULT_STACK_SIZE,     NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);

    return 0;
}
