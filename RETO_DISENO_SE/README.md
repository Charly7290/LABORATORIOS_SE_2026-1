# Reto de Diseño — Agitador de Muestras Clínico

Prototipo de sistema electrónico de agitación controlada para laboratorio clínico, desarrollado sobre **ESP32 NodeMCU** con **ESP-IDF**.

---

## Descripción

Sistema de control para un motor DC de 12V/2A que acciona un agitador mecánico de tubos de ensayo. Permite al técnico seleccionar el sentido de giro, ajustar la velocidad mediante potenciómetro y visualizar el porcentaje de potencia en tres displays de 7 segmentos.

---

## Hardware

| Componente | Función |
|---|---|
| IRF540 (×2) + IRF9540N (×2) | Puente H discreto |
| PC817 (×2) | Aislamiento galvánico control/potencia |
| 1N4007 (×4) | Protección contra back-EMF del motor |
| 3× display 7 segmentos | Visualización de potencia 000–100% |
| Potenciómetro 10kΩ | Control de velocidad |
| 2× pulsador | Selección de sentido de giro |
| LED verde / LED rojo | Indicadores de dirección |

---

## Funcionalidades

- Control PWM a 10kHz sobre puente H en topología anti-fase bloqueada
- Parada suave antes de inversión de giro (400ms rampa + 200ms freno dinámico)
- Exactamente un LED encendido en todo momento
- Displays multiplexados actualizados en tiempo real con el nivel de potencia

---

## Requisitos

- ESP-IDF v5.x
- Fuente de alimentación 12V / 2A
- ESP32 NodeMCU alimentado por USB independiente
