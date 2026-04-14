# Sistemas Embebidos — Laboratorios ESP32

Laboratorios de la asignatura **Sistemas Embebidos**, desarrollados sobre **ESP32 NodeMCU** con **ESP-IDF**.

---

## Requisitos

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- ESP32 NodeMCU
- Cable USB

---

## Compilar y flashear

Desde la carpeta de cualquier laboratorio:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Windows:** reemplazar `/dev/ttyUSB0` por `COM3` (o el puerto correspondiente).

---

## Laboratorios

### Lab 01 — Virus Defender (Matriz LED 8×8)

Juego interactivo sobre una matriz LED bicolor 8×8. El jugador recorre el perímetro del tablero disparando para eliminar virus que se propagan desde el centro antes de que alcancen el borde.

**Temas:** GPIO · Timers · Interrupciones · Multiplexación · Máquina de estados

📁 [`/LAB2_SE`](./LAB2_SE)

---

### Reto de Diseño — Agitador de Muestras Clínico
Prototipo funcional de un sistema electrónico de agitación controlada para el laboratorio clínico del Hospital Pablo Tobón Uribe. Controla un motor DC de 12V/2A mediante un puente H discreto con MOSFETs, con aislamiento galvánico por optoacopladores entre la etapa de control y la etapa de potencia.

El técnico puede seleccionar el sentido de giro del motor, ajustar la intensidad de agitación mediante un potenciómetro y visualizar el porcentaje de potencia aplicado en tres displays de 7 segmentos. El sistema incluye protección contra inversión brusca de giro mediante una secuencia de parada suave.

**Temas:** PWM · ADC · Puente H · Optoacopladores · MOSFETs · Interfaces de potencia · Protección inductiva · Multiplexación · Máquina de estados

📁 [`/RETO_DISENO_SE`](./RETO_DISENO_SE)

---

**Carlos Daniel Murillo Mena** · Sistemas Embebidos · Universidad EIA
