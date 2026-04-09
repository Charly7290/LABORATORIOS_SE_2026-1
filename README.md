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

**Carlos Daniel Murillo Mena** · Sistemas Embebidos · Universidad EIA
