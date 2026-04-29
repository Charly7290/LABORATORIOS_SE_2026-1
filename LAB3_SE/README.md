# Lab 03 — Sistema Domótico de Temperatura e Iluminación

Sistema embebido domótico para control automático de temperatura e iluminación, desarrollado sobre **ESP32 NodeMCU** con **ESP-IDF**.

---

## Descripción

Sistema que monitorea continuamente la temperatura y el nivel de iluminación de una habitación, activando actuadores de forma autónoma según las condiciones medidas. La temperatura de control se configura desde el monitor serial con el comando `SET_TEMP:XX`. El sistema permanece inactivo hasta recibir este comando por primera vez.

---

## Hardware

| Componente | Función |
|---|---|
| LM35 | Sensor de temperatura (10mV/°C) |
| Fotoresistencia + 10kΩ | Sensor de nivel de iluminación |
| Relé SRD-05VDC | Control de bombilla 12VAC 20W (calefacción) |
| Motor paso a paso 28BYJ-48 | Ventilación — 3 velocidades, 2 sentidos |
| 4× 2N2222 | Drivers de bobinas del stepper |
| 2× LED de potencia 3W | Iluminación artificial |
| IRLZ44N | Control PWM de LEDs de potencia |
| 2N2222 + BJT | Etapa de inversión lógica para LEDs |
| 4× 1N4007 | Protección back-EMF bobinas stepper |

---

## Funcionalidades

- Control de temperatura con cinco zonas: calefacción, idle, ventilación baja, media y alta
- Motor paso a paso con pasos regulables
- Seis niveles de iluminación automática según fotoresistencia (0–100%)
- Calibración manual del ADC para compensar no linealidad del ESP32
- Reporte continuo por UART cada 2 segundos: Tc, T, ni%, LED%
- Umbrales de temperatura modificables en cualquier momento desde el monitor serial

---

## Zonas de temperatura

| Condición | Calefacción | Stepper | Velocidad |
|---|---|---|---|
| T < Tc − 1°C | ON | Horario | 100 steps/s |
| Tc − 1 ≤ T ≤ Tc + 1 | OFF | Apagado | — |
| Tc + 1 < T < Tc + 3 | OFF | Antihorario | 100 steps/s |
| Tc + 3 ≤ T ≤ Tc + 5 | OFF | Antihorario | 300 steps/s |
| T > Tc + 5°C | OFF | Antihorario | 600 steps/s |

---

## Niveles de iluminación

| Nivel ambiente (ni) | Brillo LEDs |
|---|---|
| ni < 20% | 100% |
| 20% ≤ ni < 30% | 80% |
| 30% ≤ ni < 40% | 60% |
| 40% ≤ ni < 60% | 50% |
| 60% ≤ ni < 80% | 30% |
| ni ≥ 80% | 0% |

---

## Requisitos

- ESP-IDF v5.x
- Fuente de alimentación 5V / 3A (stepper + relé + LEDs)
- Transformador 12VAC (bombilla — aislado mediante relé)
- ESP32 NodeMCU alimentado por USB independiente

---

## Uso
Conectar USB → abrir monitor serial → enviar SET_TEMP:XX

El sistema arranca en silencio. Una vez recibido el primer `SET_TEMP:XX` entra en operación y reporta cada 2 segundos:
Tc=28.0 | T=27.5 C | ni=42% | LED=60%
