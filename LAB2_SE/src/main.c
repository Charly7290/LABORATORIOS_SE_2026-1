#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_random.h"

//  DEFINICIÓN DE PINES

static const gpio_num_t ROW_PINS[8] = {
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_22,
    GPIO_NUM_4,  GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_5
};
static const gpio_num_t COL_PINS[8] = {
    GPIO_NUM_14, GPIO_NUM_27, GPIO_NUM_26, GPIO_NUM_25,
    GPIO_NUM_15, GPIO_NUM_2,  GPIO_NUM_23, GPIO_NUM_13
};

#define RED_SEL    GPIO_NUM_33
#define GRN_SEL    GPIO_NUM_32
#define BTN_LEFT   GPIO_NUM_34
#define BTN_RIGHT  GPIO_NUM_39
#define BTN_SHOOT  GPIO_NUM_36

//  CONSTANTES

#define MAX_VIRUSES      12
#define MAX_BULLETS       3
#define SCORE_TO_WIN     10
#define PERIMETER        28
#define INNER_MIN         1
#define INNER_MAX         6
#define BTN_DEBOUNCE_US   300000
#define INITIAL_VIRUSES   3
#define SPREAD_INTERVAL   3500000   
#define VIRUS_MIN_AGE     3         

#define COLOR_OFF    0
#define COLOR_RED    1
#define COLOR_GREEN  2
#define COLOR_YELLOW 3

typedef enum {
    STATE_IDLE,
    STATE_PLAYING,
    STATE_WIN,
    STATE_DEAD
} game_state_t;

//  BUFFERS DE IMAGEN

static uint8_t red_buf[8];
static uint8_t grn_buf[8];

static void clear_display(void) {
    memset(red_buf, 0, 8);
    memset(grn_buf, 0, 8);
}

static void set_pixel(int r, int c, uint8_t color) {
    if (r < 0 || r > 7 || c < 0 || c > 7) return;
    uint8_t mask = (1 << (7 - c));
    red_buf[r] &= ~mask;
    grn_buf[r] &= ~mask;
    if (color == COLOR_RED    || color == COLOR_YELLOW) red_buf[r] |= mask;
    if (color == COLOR_GREEN  || color == COLOR_YELLOW) grn_buf[r] |= mask;
}

//  DRIVER DE MULTIPLEXACIÓN

static volatile uint8_t mux_row = 0;
static volatile uint8_t mux_sub = 0;

static void mux_callback(void *arg) {
    // Apagar todas las filas y selectores de color
    for (int i = 0; i < 8; i++) gpio_set_level(ROW_PINS[i], 0);
    gpio_set_level(RED_SEL, 0);
    gpio_set_level(GRN_SEL, 0);

    // Seleccionar datos según el subpaso (rojo o verde)
    uint8_t row_data = (mux_sub == 0) ? red_buf[mux_row]
                                      : grn_buf[mux_row];
    for (int c = 0; c < 8; c++)
        gpio_set_level(COL_PINS[c], (row_data >> (7 - c)) & 0x01);

    // Activar el selector de color correspondiente
    if (mux_sub == 0) gpio_set_level(RED_SEL, 1);
    else              gpio_set_level(GRN_SEL, 1);
    gpio_set_level(ROW_PINS[mux_row], 1);

    // Avanzar al siguiente subpaso o fila
    if (++mux_sub >= 2) { mux_sub = 0; mux_row = (mux_row + 1) % 8; }
}

//  SISTEMA DE PERÍMETRO

typedef struct { int row; int col; } pos_t;
typedef struct { int dr;  int dc;  } dir_t;

static pos_t perimeter_pos(int p) {
    p = ((p % PERIMETER) + PERIMETER) % PERIMETER;
    if (p <= 7)  return (pos_t){ 0,       p      };  // arriba
    if (p <= 14) return (pos_t){ p - 7,   7      };  // derecha
    if (p <= 21) return (pos_t){ 7,       21 - p };  // abajo
    return             (pos_t){ 28 - p,   0      };  // izquierda
}

static dir_t shoot_dir(int p) {
    p = ((p % PERIMETER) + PERIMETER) % PERIMETER;
    if (p <= 7)  return (dir_t){  1,  0 };  // arriba   → dispara hacia abajo
    if (p <= 14) return (dir_t){  0, -1 };  // derecha  → dispara hacia la izquierda
    if (p <= 21) return (dir_t){ -1,  0 };  // abajo    → dispara hacia arriba
    return             (dir_t){  0,  1 };  // izquierda → dispara hacia la derecha
}

//  ESTADO DEL JUEGO

typedef struct {
    int  row, col;
    int  dr, dc;
    bool active;
} bullet_t;

typedef struct {
    int     row, col;
    bool    active;
    bool    dying;
    uint8_t blink_timer;
    uint8_t age;          // ciclos de propagación sobrevividos, debe alcanzar VIRUS_MIN_AGE antes de escapar al campo interior
} virus_t;

static game_state_t game_state  = STATE_IDLE;
static int          player_pos  = 0;
static int          score       = 0;
static bullet_t     bullets[MAX_BULLETS];
static virus_t      viruses[MAX_VIRUSES];
static int          virus_count = 0;
static uint32_t     anim_timer  = 0;

//  BANDERAS VOLÁTILES

static volatile bool    flag_tick    = false;
static volatile bool    flag_spread  = false;
static volatile bool    flag_btn_l   = false;
static volatile bool    flag_btn_r   = false;
static volatile bool    flag_btn_s   = false;
static volatile int64_t last_time_l  = 0;
static volatile int64_t last_time_r  = 0;
static volatile int64_t last_time_s  = 0;

//  FUNCIONES ISR

static void IRAM_ATTR isr_left(void *arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_time_l > BTN_DEBOUNCE_US) {
        flag_btn_l  = true;
        last_time_l = now;
    }
}

static void IRAM_ATTR isr_right(void *arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_time_r > BTN_DEBOUNCE_US) {
        flag_btn_r  = true;
        last_time_r = now;
    }
}

static void IRAM_ATTR isr_shoot(void *arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_time_s > BTN_DEBOUNCE_US) {
        flag_btn_s  = true;
        last_time_s = now;
    }
}

//  CALLBACKS DE TEMPORIZADORES

static void tick_callback(void *arg)   { flag_tick   = true; }
static void spread_callback(void *arg) { flag_spread = true; }

//  FUNCIONES AUXILIARES DEL JUEGO

static bool is_perimeter(int row, int col) {
    return (row == 0 || row == 7 || col == 0 || col == 7);
}

static bool is_inner(int row, int col) {
    return (row >= INNER_MIN && row <= INNER_MAX &&
            col >= INNER_MIN && col <= INNER_MAX);
}

static void spawn_virus(void) {
    if (virus_count >= MAX_VIRUSES) return;

    // Buscar un slot libre en el arreglo de virus
    int slot = -1;
    for (int i = 0; i < MAX_VIRUSES; i++) {
        if (!viruses[i].active) { slot = i; break; }
    }
    if (slot < 0) return;

    // Intentar ubicar el virus en una celda libre del campo interior
    for (int attempt = 0; attempt < 20; attempt++) {
        int r = INNER_MIN + (esp_random() % (INNER_MAX - INNER_MIN + 1));
        int c = INNER_MIN + (esp_random() % (INNER_MAX - INNER_MIN + 1));

        bool occupied = false;
        for (int i = 0; i < MAX_VIRUSES; i++) {
            if (viruses[i].active &&
                viruses[i].row == r &&
                viruses[i].col == c) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            viruses[slot].row         = r;
            viruses[slot].col         = c;
            viruses[slot].active      = true;
            viruses[slot].dying       = false;
            viruses[slot].blink_timer = 0;
            viruses[slot].age         = 0;
            virus_count++;
            return;
        }
    }
}

//  INICIALIZACIÓN DEL JUEGO

static void game_init(void) {
    player_pos  = 0;
    score       = 0;
    virus_count = 0;
    anim_timer  = 0;

    memset(bullets, 0, sizeof(bullets));
    memset(viruses, 0, sizeof(viruses));

    // Generar los virus iniciales
    for (int i = 0; i < INITIAL_VIRUSES; i++) spawn_virus();

    printf("\n=== VIRUS DEFENDER ===\n");
    printf("IZQ=mover  DER=mover  DISPARO=fuego\n");
    printf("Elimina %d virus para ganar.\n", SCORE_TO_WIN);
    printf("¡No los dejes llegar al borde!\n\n");
}

//  LÓGICA DE BALAS

static void fire_bullet(void) {
    // Buscar una bala inactiva para disparar
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            pos_t p = perimeter_pos(player_pos);
            dir_t d = shoot_dir(player_pos);

            // Posición inicial de la bala: un paso adelante del jugador
            int br = p.row + d.dr;
            int bc = p.col + d.dc;

            if (br >= 0 && br <= 7 && bc >= 0 && bc <= 7) {
                bullets[i].row    = br;
                bullets[i].col    = bc;
                bullets[i].dr     = d.dr;
                bullets[i].dc     = d.dc;
                bullets[i].active = true;
            }
            return;
        }
    }
}

static void update_bullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;

        // Avanzar la bala en su dirección
        bullets[i].row += bullets[i].dr;
        bullets[i].col += bullets[i].dc;

        // Desactivar si sale del tablero
        if (bullets[i].row < 0 || bullets[i].row > 7 ||
            bullets[i].col < 0 || bullets[i].col > 7) {
            bullets[i].active = false;
            continue;
        }
        // Desactivar si llega al perímetro opuesto
        if (is_perimeter(bullets[i].row, bullets[i].col)) {
            bullets[i].active = false;
        }
    }
}

//  LÓGICA DE VIRUS

static void spread_viruses(void) {
    int count_before = virus_count;

    for (int i = 0; i < MAX_VIRUSES && virus_count < MAX_VIRUSES; i++) {
        if (!viruses[i].active || viruses[i].dying) continue;
        if (i >= count_before) continue;

        // Incrementar edad del virus en cada ciclo de propagación
        viruses[i].age++;

        // Un virus solo puede escapar del campo interior si:
        //   1. Está en el borde interior (fila/columna 1 o 6)
        //   2. Ha sobrevivido al menos VIRUS_MIN_AGE ciclos de propagación
        // Ambas condiciones deben cumplirse simultáneamente
        bool on_inner_border = (viruses[i].row == INNER_MIN ||
                                viruses[i].row == INNER_MAX ||
                                viruses[i].col == INNER_MIN ||
                                viruses[i].col == INNER_MAX);

        bool can_escape = on_inner_border &&
                          (viruses[i].age >= VIRUS_MIN_AGE);

        // Mezcla aleatoria para orden de direcciones
        int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (int d = 3; d > 0; d--) {
            int j    = esp_random() % (d + 1);
            int tmp0 = dirs[d][0]; int tmp1 = dirs[d][1];
            dirs[d][0] = dirs[j][0]; dirs[d][1] = dirs[j][1];
            dirs[j][0] = tmp0;       dirs[j][1] = tmp1;
        }

        for (int d = 0; d < 4; d++) {
            int nr = viruses[i].row + dirs[d][0];
            int nc = viruses[i].col + dirs[d][1];

            if (can_escape) {
                // Puede propagarse al perímetro pero no fuera de la matriz
                if (nr < 0 || nr > 7 || nc < 0 || nc > 7) continue;
            } else {
                // Debe permanecer dentro del campo interior
                if (!is_inner(nr, nc)) continue;
            }

            // Verificar si la celda destino está ocupada
            bool occ = false;
            for (int j = 0; j < MAX_VIRUSES; j++) {
                if (viruses[j].active &&
                    viruses[j].row == nr &&
                    viruses[j].col == nc) {
                    occ = true; break;
                }
            }
            if (!occ) {
                // Crear nuevo virus en la celda adyacente libre
                for (int j = 0; j < MAX_VIRUSES; j++) {
                    if (!viruses[j].active) {
                        viruses[j].row         = nr;
                        viruses[j].col         = nc;
                        viruses[j].active      = true;
                        viruses[j].dying       = false;
                        viruses[j].blink_timer = 0;
                        viruses[j].age         = 0;
                        virus_count++;
                        break;
                    }
                }
                break;
            }
        }
    }
}

static void update_dying_viruses(void) {
    for (int i = 0; i < MAX_VIRUSES; i++) {
        if (!viruses[i].active || !viruses[i].dying) continue;
        viruses[i].blink_timer--;
        // Cuando el temporizador llega a cero, eliminar el virus
        if (viruses[i].blink_timer == 0) {
            viruses[i].active = false;
            viruses[i].dying  = false;
            virus_count--;
        }
    }
}

//  DETECCIÓN DE COLISIONES

static void check_collisions(void) {
    for (int b = 0; b < MAX_BULLETS; b++) {
        if (!bullets[b].active) continue;

        for (int v = 0; v < MAX_VIRUSES; v++) {
            if (!viruses[v].active || viruses[v].dying) continue;

            // Comprobar si la bala coincide con la posición del virus
            if (bullets[b].row == viruses[v].row &&
                bullets[b].col == viruses[v].col) {

                viruses[v].dying       = true;
                viruses[v].blink_timer = 6;
                bullets[b].active      = false;
                score++;

                printf("¡Impacto! Puntuación: %d / %d\n", score, SCORE_TO_WIN);

                if (score >= SCORE_TO_WIN) {
                    game_state = STATE_WIN;
                    printf("=== ¡GANASTE! ===\n");
                }
                break;
            }
        }
    }
}

//  VERIFICACIÓN DE BORDE

static void check_edge(void) {
    for (int i = 0; i < MAX_VIRUSES; i++) {
        if (!viruses[i].active || viruses[i].dying) continue;
        // Si un virus alcanza el perímetro, el jugador pierde
        if (is_perimeter(viruses[i].row, viruses[i].col)) {
            game_state = STATE_DEAD;
            printf("=== ¡GAME OVER! Un virus llegó al borde. Puntuación: %d ===\n",
                   score);
            return;
        }
    }
}

//  FUNCIONES DE RENDERIZADO

static void render_frame(void) {
    clear_display();

    // Virus — rojo, parpadea rojo/verde al morir
    for (int i = 0; i < MAX_VIRUSES; i++) {
        if (!viruses[i].active) continue;
        if (viruses[i].dying) {
            uint8_t color = (viruses[i].blink_timer % 2 == 0) ?
                             COLOR_RED : COLOR_GREEN;
            set_pixel(viruses[i].row, viruses[i].col, color);
        } else {
            set_pixel(viruses[i].row, viruses[i].col, COLOR_RED);
        }
    }

    // Balas — verde
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;
        set_pixel(bullets[i].row, bullets[i].col, COLOR_GREEN);
    }

    // Jugador — verde
    pos_t pp = perimeter_pos(player_pos);
    set_pixel(pp.row, pp.col, COLOR_GREEN);
}

static void render_idle(void) {
    clear_display();

    // Luz de persecución de dos píxeles alrededor del perímetro
    int head = anim_timer % PERIMETER;
    for (int i = 0; i < 3; i++) {
        pos_t p = perimeter_pos((head - i + PERIMETER) % PERIMETER);
        set_pixel(p.row, p.col, COLOR_GREEN);
    }

    // Centro rojo parpadeante, presiona DISPARO para comenzar
    if ((anim_timer / 8) % 2 == 0) {
        set_pixel(3, 3, COLOR_RED);
        set_pixel(3, 4, COLOR_RED);
        set_pixel(4, 3, COLOR_RED);
        set_pixel(4, 4, COLOR_RED);
    }
}

static void render_win(void) {
    // Parpadeo de toda la pantalla en verde al ganar
    if ((anim_timer / 4) % 2 == 0) {
        memset(grn_buf, 0xFF, 8);
        memset(red_buf, 0x00, 8);
    } else {
        clear_display();
    }
}

static void render_dead(void) {
    // Parpadeo de toda la pantalla en rojo al perder
    if ((anim_timer / 4) % 2 == 0) {
        memset(red_buf, 0xFF, 8);
        memset(grn_buf, 0x00, 8);
    } else {
        clear_display();
    }
}

//  CONFIGURACIÓN DE GPIO

static void gpio_setup(void) {
    // Construir máscara de pines de salida
    uint64_t out_mask = 0;
    for (int i = 0; i < 8; i++) out_mask |= (1ULL << ROW_PINS[i]);
    for (int i = 0; i < 8; i++) out_mask |= (1ULL << COL_PINS[i]);
    out_mask |= (1ULL << RED_SEL) | (1ULL << GRN_SEL);

    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&out_cfg);

    // Inicializar todos los pines de salida en bajo
    for (int i = 0; i < 8; i++) gpio_set_level(ROW_PINS[i], 0);
    for (int i = 0; i < 8; i++) gpio_set_level(COL_PINS[i], 0);
    gpio_set_level(RED_SEL, 0);
    gpio_set_level(GRN_SEL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Configurar pines de botones como entradas con interrupción en flanco de bajada
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_LEFT)  |
                        (1ULL << BTN_RIGHT) |
                        (1ULL << BTN_SHOOT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE
    };
    gpio_config(&btn_cfg);

    // Registrar manejadores ISR para cada botón
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_LEFT,  isr_left,  NULL);
    gpio_isr_handler_add(BTN_RIGHT, isr_right, NULL);
    gpio_isr_handler_add(BTN_SHOOT, isr_shoot, NULL);
}

//  CONFIGURACIÓN DE TEMPORIZADORES

static void timer_setup(void) {

    // Temporizador de multiplexación: refresca la pantalla cada 1 ms
    esp_timer_handle_t mux_timer;
    esp_timer_create_args_t mux_args = {
        .callback        = mux_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "mux"
    };
    esp_timer_create(&mux_args, &mux_timer);
    esp_timer_start_periodic(mux_timer, 1000);

    // Temporizador de tick: actualiza la lógica del juego cada 150 ms
    esp_timer_handle_t tick_timer;
    esp_timer_create_args_t tick_args = {
        .callback        = tick_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "tick"
    };
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 150000);

    // Temporizador de propagación: expande los virus según SPREAD_INTERVAL
    esp_timer_handle_t spread_timer;
    esp_timer_create_args_t spread_args = {
        .callback        = spread_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "spread"
    };
    esp_timer_create(&spread_args, &spread_timer);
    esp_timer_start_periodic(spread_timer, SPREAD_INTERVAL);
}

//  FUNCIÓN PRINCIPAL

void app_main(void) {

    gpio_setup();
    timer_setup();

    // Esperar a que el hardware se estabilice
    vTaskDelay(pdMS_TO_TICKS(500));

    // Limpiar todas las banderas antes de entrar al bucle principal
    flag_tick   = false;
    flag_spread = false;
    flag_btn_l  = false;
    flag_btn_r  = false;
    flag_btn_s  = false;
    anim_timer  = 0;

    printf("\n=== VIRUS DEFENDER ===\n");
    printf("Presiona DISPARO para comenzar\n\n");

    while(1) {

        if (flag_tick) {
            flag_tick = false;
            anim_timer++;

            // Ejecutar lógica y renderizado según el estado actual
            switch (game_state) {
                case STATE_IDLE:
                    render_idle();
                    break;
                case STATE_PLAYING:
                    update_bullets();
                    update_dying_viruses();
                    check_collisions();
                    check_edge();
                    render_frame();
                    break;
                case STATE_WIN:
                    render_win();
                    break;
                case STATE_DEAD:
                    render_dead();
                    break;
            }
        }

        // Propagar virus solo durante la partida activa
        if (flag_spread && game_state == STATE_PLAYING) {
            flag_spread = false;
            spread_viruses();
        }

        // Botón izquierdo: mover jugador en sentido antihorario
        if (flag_btn_l) {
            flag_btn_l = false;
            if (game_state == STATE_PLAYING)
                player_pos = ((player_pos - 1) + PERIMETER) % PERIMETER;
        }

        // Botón derecho: mover jugador en sentido horario
        if (flag_btn_r) {
            flag_btn_r = false;
            if (game_state == STATE_PLAYING)
                player_pos = (player_pos + 1) % PERIMETER;
        }

        // Botón de disparo: iniciar partida, disparar o reiniciar
        if (flag_btn_s) {
            flag_btn_s = false;
            if (game_state == STATE_IDLE) {
                game_init();
                game_state = STATE_PLAYING;
            } else if (game_state == STATE_PLAYING) {
                fire_bullet();
            } else if (game_state == STATE_WIN ||
                       game_state == STATE_DEAD) {
                game_state = STATE_IDLE;
                anim_timer = 0;
                clear_display();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}