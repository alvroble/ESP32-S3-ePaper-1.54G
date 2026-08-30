# 🪙 Bitcoin E-Paper Tracker (ESP32-S3 + 1.54" 4-Color E-Paper)

Dispositivo autónomo de seguimiento de Bitcoin en tiempo real con pantalla de tinta electrónica de 1.54 pulgadas (200×200 px), gráfico histórico de velas de 24 horas, cálculo en vivo de Satoshis/USD, medición de batería calibrada por ADC y modo **Deep Sleep** de bajo consumo.

---

## 📌 Especificaciones de Hardware

| Componente | Detalle |
| :--- | :--- |
| **Microcontrolador** | ESP32-S3 (4MB Flash, 8MB Octal PSRAM/SPIRAM) |
| **Pantalla** | 1.54" E-Paper 4 colores (Negro, Blanco, Amarillo, Rojo) - 200 × 200 píxeles |
| **Frameworks** | ESP-IDF v5.5.1 + LVGL v9.2.2 |
| **Conectividad** | Wi-Fi 2.4 GHz + TLS 1.3 con bundle de certificados X.509 |

---

## ⚡ Asignación de Pines y Control de Energía

| Pin (GPIO) | Función | Descripción |
| :--- | :--- | :--- |
| **GPIO 17** (`VBAT_PWR_PIN`) | **Enclavamiento de Batería (Power Latch)** | Debe ponerse a **Nivel ALTO (`HIGH / 1`)** inmediatamente al arrancar y mantenerse enclavado en Deep Sleep (`gpio_hold_en`) para que la batería no se desconecte. |
| **GPIO 6** (`EPD_PWR_PIN`) | **Alimentación de Pantalla E-Paper** | Nivel `0` = Pantalla encendida. Nivel `1` = Pantalla apagada (0 consumo). |
| **GPIO 4** (`ADC_CHANNEL_3`) | **Lectura de Batería (ADC1)** | Conectado a un divisor de tensión resistivo $1/2$. Se lee con calibración `esp_adc` y se multiplica por 2. |
| **GPIO 0** (`BOOT_BUTTON_PIN`) | **Botón BOOT / Despertar Manual** | Configurado como `ext0_wakeup` a nivel `0`. Al pulsarlo en Deep Sleep, fuerza un refresco inmediato sin esperar los 5 min. |
| **GPIO 18** (`PWR_BUTTON_PIN`) | **Botón de Encendido Hardware** | Pulso inicial para alimentar el circuito con batería antes de que el GPIO 17 tome el relevo. |

---

## 🏗️ Arquitectura de Software

```
┌─────────────────────────────────────────────────────────────┐
│ 1. ARRANQUE (user_app_init)                                 │
│    - Activa GPIO 17 (retención de batería).                 │
│    - Inicializa LVGL v9 y renderizador e-Paper.             │
│    - Inicia conexión Wi-Fi (WIFI_PS_NONE).                  │
├─────────────────────────────────────────────────────────────┤
│ 2. CICLO DE ACTUALIZACIÓN (~3.5 s)                          │
│    - Sincroniza reloj UTC vía SNTP (pool.ntp.org).          │
│    - Consulta velas de 24h a Coinbase Exchange:             │
│      GET https://api.exchange.coinbase.com/products/...     │
│    - Parsea 24 velas horarias (Close, High, Low, Open).     │
│    - Actualiza interfaz LVGL:                               │
│      * Precio grande ($ 79,209)                             │
│      * Conversión Sats/USD (1,262 sats/$)                   │
│      * Gráfico Sparkline continuo (24 puntos reales)        │
│      * Variación 24h real (%) y picos High/Low              │
│      * Hora exacta de actualización en UTC                  │
│      * Medición analógica real de batería / USB             │
│    - Convierte framebuffer con umbral de luminancia nítido. │
│    - Envía la imagen al e-Paper y apaga su alimentación.    │
├─────────────────────────────────────────────────────────────┤
│ 3. DEEP SLEEP (5 minutos / 300 s)                           │
│    - Apaga la radio Wi-Fi (esp_wifi_stop).                  │
│    - Enclava GPIO 17 en Deep Sleep (gpio_deep_sleep_hold).  │
│    - Programa temporizador de 5 minutos y botón GPIO 0.     │
│    - Consumo en reposo: ~35 µA.                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 🔋 Estimación de Autonomía de Batería

* **Consumo activo**: ~95 mA durante ~3.5 segundos por ciclo.
* **Consumo en reposo (Deep Sleep)**: ~35 µA (0.035 mA).
* **Consumo diario (288 ciclos de 5 min)**: **~31 mAh / día** (consumo medio equivalente a 1.3 mA).

| Capacidad Batería LiPo | Autonomía (Refresco cada 5 min) | Autonomía (Refresco cada 15 min) |
| :--- | :--- | :--- |
| **500 mAh** | **~15 a 16 días** | **~40 días** |
| **1000 mAh** | **~30 a 32 días** (~1 mes) | **~80 días** |
| **2000 mAh** | **~60 días** (~2 meses) | **~150 días** (~5 meses) |

---

## 🚀 Compilación y Flasheo

1. **Cargar el entorno de ESP-IDF**:
   ```bash
   . /Users/alvrb/esp/esp-idf/export.sh
   ```

2. **Compilar**:
   ```bash
   idf.py build
   ```

3. **Flashear y monitorizar**:
   ```bash
   idf.py flash monitor
   ```

---

## 💡 Ideas para Futuras Mejoras

- [ ] **Portal cautivo / BLE Provisioning**: Configurar las credenciales Wi-Fi desde el móvil sin recompilar el código.
- [ ] **Selector de Criptomoneda**: Alternar entre Bitcoin (BTC), Ethereum (ETH), Solana (SOL) mediante pulsaciones del botón BOOT.
- [ ] **Modo Nocturno / Horario de sueño**: Reducir la frecuencia de refresco (ej. cada 1 hora) durante la noche para duplicar la duración de la batería.
- [ ] **Conversión a EUR/USD**: Parámetro para alternar entre dólares ($) y euros (€).
- [ ] **Refresco parcial (Partial Refresh)**: Actualizar la pantalla sin parpadeo blanco/negro completo.
