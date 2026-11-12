#include <stdio.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>
#include <semphr.h>

#include "tkjhat/sdk.h"

#define IMU_TASK_STACK_SIZE 1024
#define DEFAULT_STACK_SIZE 2048
#define SERIAL_TASK_STACK_SIZE 1024
#define SERIAL_BUFFER_SIZE 256
#define DISPLAY_TASK_STACK_SIZE 1024
#define DISPLAY_TEXT_BUFFER_SIZE 256

static char display_text[DISPLAY_TEXT_BUFFER_SIZE] = "Waiting...";
static SemaphoreHandle_t display_mutex;
uint8_t button_pressed = 0;

//button interrupt handler
static void btn_fxn(uint gpio, uint32_t eventMask) {
    //Debounce the button press by checking the time since the last press
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

//ImuTask that reads the sensor data and prints it to the serial port
static void imuTask(void *pvParameters) {
    (void)pvParameters;
    float gx, gy, gz, ay, ax, az, t;
    
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        ICM42670_read_sensor_data(&ax, &ay, &az, &gx, &gy, &gz, &t);
        //flushesh the buffer :)
        stdio_flush();
        vTaskDelay(pdMS_TO_TICKS(500));

        printf(">gx:%.2f,gy:%.2f,gz:%.2f\n", gx, gy, gz);
        

        vTaskDelay(pdMS_TO_TICKS(1000));

        // printf("Gyro: gx=%.2f, gy=%.2f, gz=%.2f\n", gx, gy, gz);

    }
}
//Serial task that reads the serial input and prints it to the serial port
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

            if (xSemaphoreTake(display_mutex, portMAX_DELAY) == pdTRUE) {
            strncpy(display_text, message, DISPLAY_TEXT_BUFFER_SIZE);
            display_text[DISPLAY_TEXT_BUFFER_SIZE - 1] = '\0';
            xSemaphoreGive(display_mutex);
}

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

static void displayTask(void *pvParameters) {
    (void)pvParameters;

    init_display();

    while (1) {
        char local_text[DISPLAY_TEXT_BUFFER_SIZE] = "";

        if (xSemaphoreTake(display_mutex, portMAX_DELAY) == pdTRUE) {
            strncpy(local_text, display_text, DISPLAY_TEXT_BUFFER_SIZE);
            local_text[DISPLAY_TEXT_BUFFER_SIZE - 1] = '\0';
            xSemaphoreGive(display_mutex);
        }

        clear_display();
        write_text_xy(0, 0, local_text);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}



//main function that initializes the SDK, the sensor, the buttons, and creates the tasks
int main() {
    stdio_init_all();
    init_hat_sdk();
    display_mutex = xSemaphoreCreateMutex();

    if (display_mutex == NULL) {
        return 1;
    }

    sleep_ms(300);

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

    // Rekisteröidään keskeytykset napeille SW1_PIN ja SW2_PIN
    gpio_set_irq_enabled_with_callback(SW1_PIN, GPIO_IRQ_EDGE_FALL, true, btn_fxn);
    gpio_set_irq_enabled(SW2_PIN, GPIO_IRQ_EDGE_FALL, true);

    sleep_ms(50);

    // Create imutask and serial task
    BaseType_t result = xTaskCreate(imuTask, "IMU Task", IMU_TASK_STACK_SIZE, NULL, 1, NULL);
    BaseType_t serialResult = xTaskCreate(serialTask, "Serial Task", SERIAL_TASK_STACK_SIZE, NULL, 1, NULL);
    BaseType_t printResult = xTaskCreate(print_task, "Print Task", DEFAULT_STACK_SIZE, NULL, 1, NULL);
    BaseType_t displayResult = xTaskCreate(display_task, "Display Task", DISPLAY_TASK_STACK_SIZE, NULL, 1, NULL);

    vTaskStartScheduler();

    while (1);

    return 0;
}