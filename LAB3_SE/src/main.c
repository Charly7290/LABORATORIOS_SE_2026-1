#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"

// Defino los pines GPIO y Canales ADC usados 

#define LM35_CHANNEL    ADC1_CHANNEL_0   
#define LDR_CHANNEL     ADC1_CHANNEL_3   

#define RELAY_PIN       GPIO_NUM_32

#define LED_PWM_PIN     GPIO_NUM_33

#define STEPPER_IN1     GPIO_NUM_16
#define STEPPER_IN2     GPIO_NUM_17
#define STEPPER_IN3     GPIO_NUM_18
#define STEPPER_IN4     GPIO_NUM_19

#define UART_NUM        UART_NUM_0
#define UART_BUF_SIZE   256

// Defino constantes para el control del sistema

#define PWM_FREQ_HZ       1000
#define PWM_RESOLUTION    LEDC_TIMER_10_BIT
#define PWM_MAX_DUTY      1023

#define DIR_CW            1
#define DIR_CCW          -1
#define DIR_STOP          0

#define SPEED_LOW         100
#define SPEED_MED         300
#define SPEED_HIGH        600

// Variables globales protegidas por mutex

static SemaphoreHandle_t state_mutex;

static float   Tc            = 25.0f;
static float   T             = 0.0f;
static uint8_t ni_percent    = 0;
static uint8_t led_percent   = 0;
static int     stepper_dir   = DIR_STOP;
static int     stepper_speed = SPEED_LOW;
static bool    relay_on      = false;
static bool    system_ready  = false;   // verdadero después de recibir el primer comando SET_TEMP válido

// Secuencia de pasos para el motor paso a paso (4 pasos, 4 pines)

static const gpio_num_t stepper_pins[4] = {
    STEPPER_IN1, STEPPER_IN2, STEPPER_IN3, STEPPER_IN4
};

static const uint8_t step_seq[4][4] = {
    {1, 0, 0, 0},   
    {0, 1, 0, 0},   
    {0, 0, 1, 0},   
    {0, 0, 0, 1},   
};

static int8_t current_step = 0;

static void stepper_write(int8_t step) {
    for(int p = 0; p < 4; p++)
        gpio_set_level(stepper_pins[p], step_seq[step][p]);
}

static void stepper_all_off(void) {
    for(int p = 0; p < 4; p++)
        gpio_set_level(stepper_pins[p], 0);
}

// Función para configurar el brillo de los LEDs usando PWM

static void set_led_brightness(uint8_t percent) {
    uint32_t duty = ((100 - percent) * PWM_MAX_DUTY) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Función para leer el valor promedio de un canal ADC, promediando 16 muestras para mayor estabilidad

static uint32_t adc_read_avg(adc1_channel_t channel) {
    uint32_t sum = 0;
    for(int i = 0; i < 16; i++) sum += adc1_get_raw(channel);
    return sum / 16;
}

static float read_temperature(void) {
    uint32_t raw = adc_read_avg(LM35_CHANNEL);
    return (float)(raw * 5447) / (4095.0f * 10.0f);
}

// Función para leer la iluminación y convertirla a porcentaje (0-100%), asumiendo que el LDR está conectado con un divisor de tensión y que 4095 = 0% iluminación, 0 = 100% iluminación

static uint8_t read_illumination(void) {
    uint32_t raw = adc_read_avg(LDR_CHANNEL);
    return (uint8_t)(100 - (raw * 100) / 4095);
}

// Función para mapear el porcentaje de iluminación a un porcentaje de brillo para los LEDS

static uint8_t get_led_percent(uint8_t ni) {
    if      (ni < 20) return 100;
    else if (ni < 30) return 80;
    else if (ni < 40) return 60;
    else if (ni < 60) return 50;
    else if (ni < 80) return 30;
    else              return 0;
}

// Función para actualizar el estado del sistema (rele, dirección y velocidad del motor) según la temperatura actual y la temperatura de consigna, con un margen de 1 grado. El rele se activa para calefacción (T < Tc - 1), el motor se activa para ventilación (T > Tc + 1) con diferentes velocidades según el rango de temperatura. Se imprime el estado actual en cada actualización.

static void update_thermal_state(float temp, float tc) {
    bool new_relay = false;
    int  new_dir   = DIR_STOP;
    int  new_speed = SPEED_LOW;

    if (temp < (tc - 1.0f)) {
        new_relay = true;
        new_dir   = DIR_CW;
        new_speed = SPEED_LOW;
        printf("  State: HEATING | relay ON | stepper CW 100s/s\n");

    } else if (temp <= (tc + 1.0f)) {
        new_relay = false;
        new_dir   = DIR_STOP;
        printf("  State: IDLE | all off\n");

    } else if (temp < (tc + 3.0f)) {
        new_relay = false;
        new_dir   = DIR_CCW;
        new_speed = SPEED_LOW;
        printf("  State: VENT LOW | stepper CCW 100s/s\n");

    } else if (temp <= (tc + 5.0f)) {
        new_relay = false;
        new_dir   = DIR_CCW;
        new_speed = SPEED_MED;
        printf("  State: VENT MED | stepper CCW 300s/s\n");

    } else {
        new_relay = false;
        new_dir   = DIR_CCW;
        new_speed = SPEED_HIGH;
        printf("  State: VENT HIGH | stepper CCW 600s/s\n");
    }

    gpio_set_level(RELAY_PIN, new_relay ? 1 : 0);

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    relay_on      = new_relay;
    stepper_dir   = new_dir;
    stepper_speed = new_speed;
    xSemaphoreGive(state_mutex);
}

// Task 1: Se encarga de leer los sensores, actualizar el estado del sistema y controlar el brillo de los LEDs. Se ejecuta en CPU0 y se bloquea con vTaskDelay para liberar la CPU cuando no está haciendo nada.

static void sensor_task(void *arg) {
    uint8_t print_counter = 0;

    while(1) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool ready = system_ready;
        xSemaphoreGive(state_mutex);

        if(!ready) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        float   temp = read_temperature();
        uint8_t ni   = read_illumination();
        uint8_t lp   = get_led_percent(ni);

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        float tc    = Tc;
        T           = temp;
        ni_percent  = ni;
        led_percent = lp;
        xSemaphoreGive(state_mutex);

        set_led_brightness(lp);
        update_thermal_state(temp, tc);

        // Imprime el estado cada 4 iteraciones (2 segundos) para no saturar la salida serial
        print_counter++;
        if(print_counter >= 4) {
            printf("Tc=%.1f | T=%.1f C | ni=%d%% | LED=%d%%\n",
                   tc, temp, ni, lp);
            print_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Task 2: Se encarga de controlar el motor paso a paso según la dirección y velocidad establecidas por el task de sensores. Se ejecuta en CPU1 y utiliza un bucle de espera activa (busy-wait) para garantizar un control preciso del tiempo entre pasos, sin ser interrumpido por otras tareas.

static void stepper_task(void *arg) {
    while(1) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        int dir   = stepper_dir;
        int speed = stepper_speed;
        xSemaphoreGive(state_mutex);

        if(dir == DIR_STOP) {
            stepper_all_off();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        current_step -= dir;
        if(current_step > 3) current_step = 0;
        if(current_step < 0) current_step = 3;

        stepper_write(current_step);
        esp_rom_delay_us(1000000 / speed);
    }
}

// Task 3: Se encarga de recibir comandos por UART para actualizar la temperatura de consigna (Tc). Se ejecuta en CPU0 y se bloquea esperando datos en la UART. Cuando recibe un comando válido, actualiza Tc y desbloquea el sistema si es la primera vez.

static void uart_rx_task(void *arg) {
    char    buf[64];
    uint8_t idx = 0;
    uint8_t byte;

    while(1) {
        int len = uart_read_bytes(UART_NUM, &byte, 1, portMAX_DELAY);
        if(len <= 0) continue;

        uart_write_bytes(UART_NUM, (const char*)&byte, 1);

        if(byte == '\n' || byte == '\r') {
            buf[idx] = '\0';

            if(strncmp(buf, "SET_TEMP:", 9) == 0) {
                float new_tc = atof(buf + 9);
                if(new_tc >= 0.0f && new_tc <= 99.0f) {
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    Tc           = new_tc;
                    system_ready = true;   
                    xSemaphoreGive(state_mutex);
                    printf("\nTc updated to %.1f C\n", new_tc);
                } else {
                    printf("\nInvalid temperature. Range: 0-99 C\n");
                }
            } else if(idx > 0) {
                printf("\nUnknown command. Use: SET_TEMP:XX\n");
            }

            idx = 0;

        } else if(idx < sizeof(buf) - 1) {
            buf[idx++] = (char)byte;
        }
    }
}

// GPIO SETUP

static void gpio_setup(void) {
    uint64_t out_mask =
        (1ULL << RELAY_PIN)   |
        (1ULL << STEPPER_IN1) |
        (1ULL << STEPPER_IN2) |
        (1ULL << STEPPER_IN3) |
        (1ULL << STEPPER_IN4);

    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&out_cfg);

    gpio_set_level(RELAY_PIN,   0);
    gpio_set_level(STEPPER_IN1, 0);
    gpio_set_level(STEPPER_IN2, 0);
    gpio_set_level(STEPPER_IN3, 0);
    gpio_set_level(STEPPER_IN4, 0);
}

// ADC SETUP

static void adc_setup(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LM35_CHANNEL, ADC_ATTEN_DB_12);
    adc1_config_channel_atten(LDR_CHANNEL,  ADC_ATTEN_DB_12);
}

// PWM SETUP

static void pwm_setup(void) {
    ledc_timer_config_t timer = {
        .freq_hz         = PWM_FREQ_HZ,
        .duty_resolution = PWM_RESOLUTION,
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = LED_PWM_PIN,
        .duty       = PWM_MAX_DUTY,   
        .channel    = LEDC_CHANNEL_0,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel  = LEDC_TIMER_0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch);
}

// UART SETUP

static void uart_setup(void) {
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM, &cfg);
    uart_driver_install(UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0);
}

// APP MAIN

void app_main(void) {

    gpio_setup();
    adc_setup();
    pwm_setup();
    uart_setup();

    state_mutex = xSemaphoreCreateMutex();

    // Configuro el Watchdog Timer para reiniciar el sistema si alguna tarea se queda colgada. Se monitorea solo la CPU0, ya que la CPU1 está dedicada al motor paso a paso con busy-waiting.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 5000,
        .idle_core_mask = (1 << 0),   
        .trigger_panic  = false
    };
    esp_task_wdt_reconfigure(&wdt_cfg);

    printf("\n=== Domotics system — Lab 3 ===\n");
    printf("Send SET_TEMP:XX to start the system\n\n");

    // CPU0: sensor_task y uart_rx_task, CPU1: stepper_task
    xTaskCreatePinnedToCore(sensor_task,  "sensor",  4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(stepper_task, "stepper", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx", 4096, NULL, 1, NULL, 0);
}