#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_timer.h"
#include "driver/ledc.h"

//  PINES

static const gpio_num_t SEG_PINS[7] = {
    GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19,
    GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_23
};

#define DIG_HUNDREDS  GPIO_NUM_4
#define DIG_TENS      GPIO_NUM_13
#define DIG_UNITS     GPIO_NUM_14

#define LED_GREEN     GPIO_NUM_25
#define LED_RED       GPIO_NUM_33

// Control del puente H mediante optoacopladores
// GPIO32 → PC817_L → compuerta Q1 + compuerta Q3 = semipuente izquierdo
// GPIO2  → PC817_R → compuerta Q2 + compuerta Q4 = semipuente derecho
#define HBRIDGE_L     GPIO_NUM_32   // semipuente izquierdo (sentido horario)
#define HBRIDGE_R     GPIO_NUM_2    // semipuente derecho (sentido antihorario)

#define BTN_CW        GPIO_NUM_27
#define BTN_CCW       GPIO_NUM_26

#define POT_CHANNEL   ADC1_CHANNEL_0   // GPIO36 (VP)

//  CONSTANTES

#define BTN_DEBOUNCE_US   300000

#define PWM_FREQ_HZ       10000              
#define PWM_RESOLUTION    LEDC_TIMER_10_BIT  // resolución de 0 a 1023
#define PWM_MAX_DUTY      1023

// Parada suave — protege el motor y el circuito de una inversión brusca
// Tiempo total de protección = RAMP_STEPS × RAMP_STEP_MS + BRAKE_MS
//                            = 20 × 20ms + 200ms = 600ms
#define RAMP_STEPS        20
#define RAMP_STEP_MS      20
#define BRAKE_MS          200

//  CODIFICACIÓN DE SEGMENTOS — cátodo común
//  bit0=a, bit1=b, bit2=c, bit3=d, bit4=e, bit5=f, bit6=g

static const uint8_t digit_map[10] = {
    0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111
};

//  MÁQUINA DE ESTADOS

typedef enum {
    STATE_STOPPED,    // motor apagado, esperando que se presione un botón
    STATE_CW,         // motor girando en sentido horario
    STATE_CCW,        // motor girando en sentido antihorario
    STATE_STOPPING    // bajando la velocidad antes de cambiar de dirección
} motor_state_t;

static motor_state_t motor_state   = STATE_STOPPED;
static uint8_t       display_val   = 0;     // valor de 0 a 100, mostrado en el display
static uint32_t      current_duty  = 0;     // ciclo de trabajo PWM actual, de 0 a 1023

//  BANDERAS VOLÁTILES

static volatile bool    flag_btn_cw   = false;
static volatile bool    flag_btn_ccw  = false;
static volatile int64_t last_time_cw  = 0;
static volatile int64_t last_time_ccw = 0;

//  FUNCIONES ISR

static void IRAM_ATTR isr_cw(void *arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_time_cw > BTN_DEBOUNCE_US) {
        flag_btn_cw  = true;
        last_time_cw = now;
    }
}

static void IRAM_ATTR isr_ccw(void *arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_time_ccw > BTN_DEBOUNCE_US) {
        flag_btn_ccw  = true;
        last_time_ccw = now;
    }
}

//  CALLBACK DE MULTIPLEXACIÓN 
//  Enciende un dígito del display por llamada

static volatile uint8_t mux_digit = 0;

static void mux_callback(void *arg) {
    // Apagar todos los dígitos antes de cambiar
    gpio_set_level(DIG_HUNDREDS, 0);
    gpio_set_level(DIG_TENS,     0);
    gpio_set_level(DIG_UNITS,    0);

    mux_digit = (mux_digit + 1) % 3;

    // Extraer el dígito correspondiente según la posición
    uint8_t val;
    switch (mux_digit) {
        case 0: val = display_val / 100;       break;  // centenas 
        case 1: val = (display_val / 10) % 10; break;  // decenas
        case 2: val = display_val % 10;         break;  // unidades
        default: val = 0; break;
    }

    // Enviar el patrón de segmentos al display
    uint8_t pattern = digit_map[val];
    for (uint8_t seg = 0; seg < 7; seg++)
        gpio_set_level(SEG_PINS[seg], (pattern >> seg) & 0x01);

    // Activar el dígito correspondiente
    switch (mux_digit) {
        case 0: gpio_set_level(DIG_HUNDREDS, 1); break;
        case 1: gpio_set_level(DIG_TENS,     1); break;
        case 2: gpio_set_level(DIG_UNITS,    1); break;
    }
}

//  CONTROL PWM 

static void motor_cw(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void motor_ccw(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void motor_brake(void) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void motor_coast(void) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, PWM_MAX_DUTY);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, PWM_MAX_DUTY);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}


static void soft_stop(void) {
    printf("  Parada suave: reduciendo desde ciclo de trabajo %lu...\n", current_duty);
    motor_state = STATE_STOPPING;

    uint32_t start_duty = current_duty;

    // Bajar el ciclo de trabajo progresivamente en RAMP_STEPS pasos
    for (int step = RAMP_STEPS; step >= 0; step--) {
        uint32_t duty = (start_duty * (uint32_t)step) / RAMP_STEPS;

        if (motor_state == STATE_STOPPING) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        }
        vTaskDelay(pdMS_TO_TICKS(RAMP_STEP_MS));
    }

    // Aplicar freno dinámico durante el tiempo de espera
    motor_brake();
    current_duty = 0;

    printf("  Parada suave: frenando por %dms...\n", BRAKE_MS);
    vTaskDelay(pdMS_TO_TICKS(BRAKE_MS));
    printf("  Parada suave: completada — listo para nueva dirección\n");
}

//  LEDs INDICADORES
//  Siempre debe haber exactamente un LED encendido durante la operación del sistema

static void set_direction_leds(motor_state_t state) {
    switch (state) {
        case STATE_CW:
        case STATE_STOPPING:
            // Durante la parada suave se mantiene el verde, ya que la dirección de destino es horaria. Se sobreescribe cuando arranca el antihorario.
            gpio_set_level(LED_GREEN, 1);
            gpio_set_level(LED_RED,   0);
            break;
        case STATE_CCW:
            gpio_set_level(LED_GREEN, 0);
            gpio_set_level(LED_RED,   1);
            break;
        case STATE_STOPPED:
            // Siempre debe haber un LED encendido — se conserva el último estado.
            // Por defecto el verde queda encendido al arrancar.
            break;
    }
}

//  ADC — promedio de 8 muestras, retorna valor de 0 a 100

static uint8_t read_pot_percent(void) {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += adc1_get_raw(POT_CHANNEL);
    return (uint8_t)((sum / 8 * 100) / 4095);
}

//  CONFIGURACIÓN DE GPIO

static void gpio_setup(void) {
    // Construir máscara con todos los pines de salida
    uint64_t out_mask = 0;
    for (int i = 0; i < 7; i++) out_mask |= (1ULL << SEG_PINS[i]);
    out_mask |= (1ULL << DIG_HUNDREDS) | (1ULL << DIG_TENS)  |
                (1ULL << DIG_UNITS)    | (1ULL << LED_GREEN) |
                (1ULL << LED_RED);

    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&out_cfg);

    // Inicializar todas las salidas en bajo para un arranque seguro
    for (int i = 0; i < 7; i++) gpio_set_level(SEG_PINS[i], 0);
    gpio_set_level(DIG_HUNDREDS, 0); gpio_set_level(DIG_TENS,   0);
    gpio_set_level(DIG_UNITS,    0); gpio_set_level(LED_GREEN,  0);
    gpio_set_level(LED_RED,      0);

    // Botones con pull-up interno
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_CW) | (1ULL << BTN_CCW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE
    };
    gpio_config(&btn_cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_CW,  isr_cw,  NULL);
    gpio_isr_handler_add(BTN_CCW, isr_ccw, NULL);
}

//  CONFIGURACIÓN PWM 

static void pwm_setup(void) {
    ledc_timer_config_t timer = {
        .freq_hz         = PWM_FREQ_HZ,
        .duty_resolution = PWM_RESOLUTION,
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch0 = {
        .gpio_num   = HBRIDGE_L,
        .duty       = PWM_MAX_DUTY,  
        .channel    = LEDC_CHANNEL_0,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel  = LEDC_TIMER_0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch0);

    ledc_channel_config_t ch1 = {
        .gpio_num   = HBRIDGE_R,
        .duty       = PWM_MAX_DUTY,   
        .channel    = LEDC_CHANNEL_1,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel  = LEDC_TIMER_0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch1);

    // Estado inicial seguro
    motor_coast();
}

//  CONFIGURACIÓN ADC

static void adc_setup(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(POT_CHANNEL, ADC_ATTEN_DB_11);
}

//  CONFIGURACIÓN DE TEMPORIZADORES

static void timer_setup(void) {
    esp_timer_handle_t mux_timer;
    esp_timer_create_args_t args = {
        .callback        = mux_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "mux"
    };
    esp_timer_create(&args, &mux_timer);
    esp_timer_start_periodic(mux_timer, 2000);  
}

//  FUNCIÓN PRINCIPAL

void app_main(void) {

    gpio_setup();
    adc_setup();
    pwm_setup();
    timer_setup();

    // Estado inicial seguro: motor apagado, LED verde encendido por defecto
    motor_state = STATE_STOPPED;
    motor_coast();
    gpio_set_level(LED_GREEN, 1);
    gpio_set_level(LED_RED,   0);

    printf("\n=== Agitador de muestras de sangre — controlador de motor ===\n");
    printf("BTN_CW  (GPIO27) = sentido horario        → LED verde\n");
    printf("BTN_CCW (GPIO26) = sentido antihorario    → LED rojo\n");
    printf("Potenciómetro    = velocidad 000–100%%\n");
    printf("Motor apagado al iniciar — presiona un botón para comenzar\n\n");

    while(1) {

        // Leer potenciómetro 
        uint8_t pct = read_pot_percent();

        if (pct != display_val) {
            display_val  = pct;
            current_duty = ((uint32_t)pct * PWM_MAX_DUTY) / 100;

            // Actualizar el canal activo en tiempo real, sin necesidad de detener el motor
            if (motor_state == STATE_CW)
                motor_cw(current_duty);
            else if (motor_state == STATE_CCW)
                motor_ccw(current_duty);

            printf("Potencia: %3d%%\n", pct);
        }

        // Botón horario presionado
        if (flag_btn_cw) {
            flag_btn_cw = false;

            if (motor_state == STATE_CW) {
                printf("Ya está girando en sentido horario\n");

            } else if (motor_state == STATE_CCW) {
                // Hay que hacer parada suave antes de invertir 
                printf("Invirtiendo: antihorario → horario\n");
                set_direction_leds(STATE_CW);   // mostrar la nueva dirección de inmediato
                soft_stop();
                motor_state = STATE_CW;
                motor_cw(current_duty);
                printf("Girando en sentido horario al %d%%\n", display_val);

            } else {
                // Estaba detenido : arrancar directamente
                motor_state = STATE_CW;
                set_direction_leds(STATE_CW);
                motor_cw(current_duty);
                printf("Arrancando en sentido horario al %d%%\n", display_val);
            }
        }

        // Botón antihorario presionado 
        if (flag_btn_ccw) {
            flag_btn_ccw = false;

            if (motor_state == STATE_CCW) {
                printf("Ya está girando en sentido antihorario\n");

            } else if (motor_state == STATE_CW) {
                printf("Invirtiendo: horario → antihorario\n");
                set_direction_leds(STATE_CCW);
                soft_stop();
                motor_state = STATE_CCW;
                motor_ccw(current_duty);
                printf("Girando en sentido antihorario al %d%%\n", display_val);

            } else {
                motor_state = STATE_CCW;
                set_direction_leds(STATE_CCW);
                motor_ccw(current_duty);
                printf("Arrancando en sentido antihorario al %d%%\n", display_val);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}