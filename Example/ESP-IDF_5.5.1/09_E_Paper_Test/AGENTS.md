# Waveshare ESP32-S3 ePaper 1.54G - Reglas y Directrices de Hardware

Este documento describe las reglas críticas de hardware y arquitectura para cualquier proyecto que utilice la placa **Waveshare ESP32-S3 ePaper 1.54G** (y placas compatibles de la familia Waveshare ePaper). Aplica a cualquier agente o modelo de IA (OpenCode, Claude Code, Cursor, Aider, Antigravity, etc.).

---

## 1. Pinout y Control de Energía (Imprescindible para Batería)

- **GPIO 17 (`VBAT_PWR_PIN`) - Retención de Batería (Power Latch)**:
  - La placa cuenta con un circuito de conmutación MOSFET para la batería LiPo.
  - **REGLA OBLIGATORIA**: El firmware **DEBE poner inmediatamente `GPIO 17 = 1` (`HIGH`)** en `user_app_init()` / `app_main()` para enclavar la alimentación.
  - Al entrar en Deep Sleep, se debe retener el pin con `gpio_hold_en(GPIO_NUM_17)` y `gpio_deep_sleep_hold_en()` para no cortar la alimentación de la placa.
  - Si `GPIO 17` no se activa a nivel ALTO, la placa se apagará en cuanto el usuario suelte el botón `PWR` o desconecte el cable USB.

- **GPIO 6 (`EPD_PWR_PIN`) - Alimentación Pantalla E-Paper**:
  - Control de alimentación independiente de la pantalla.
  - Nivel `0` (LOW) = Pantalla encendida.
  - Nivel `1` (HIGH) = Pantalla apagada (consumo cero en reposo).

- **GPIO 4 (`ADC_CHANNEL_3` en ADC1) - Lectura Analógica de Batería**:
  - Conectado a un divisor resistivo $1/2$ desde `VBAT`.
  - Debe inicializarse con `esp_adc` usando calibración por curva (`adc_cali_create_scheme_curve_fitting`).
  - Tensión real $= V_{\text{ADC}} \times 2.0$.
  - Rango LiPo: $3.30\text{V}$ (0%) a $4.20\text{V}$ (100%). $V \ge 4.30\text{V}$ indica alimentación por USB.

- **GPIO 0 (`BOOT_BUTTON_PIN`) - Botón de Usuario y Despertar Manual**:
  - Conectado al botón BOOT. Configurado como `esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)` para forzar refrescos manuales en Deep Sleep.

- **GPIO 18 (`PWR_BUTTON_PIN`) - Botón de Encendido Hardware**:
  - Botón pulsador que alimenta el circuito inicialmente hasta que el microcontrolador enclava el GPIO 17.

---

## 2. Renderizado en E-Paper 4 Colores (200×200 px) con LVGL v9

- **Conversión de color RGB565 a E-Paper (0x0 Negro, 0x1 Blanco, 0x2 Amarillo, 0x3 Rojo)**:
  - LVGL genera *anti-aliasing* en escala de grises que puede confundirse con amarillo o rojo si no se filtra la saturación.
  - **Algoritmo de conversión nítido**:
    1. Calcular saturación: $S = \max(R, G, B) - \min(R, G, B)$.
    2. Si $S < 45$ (tonos neutros de fuentes y bordes), aplicar umbral estricto de luminancia:
       - Luminancia $Y = (299 R + 587 G + 114 B) / 1000$.
       - Si $Y < 160 \rightarrow$ Negro (`0x0`).
       - Si $Y \ge 160 \rightarrow$ Blanco (`0x1`).
    3. Si $S \ge 45$, evaluar colores vivos (Rojo o Amarillo).

- **Legibilidad y Alineación**:
  - Utilizar fuentes de tamaño $\ge \text{Montserrat 12}$ para evitar trazos cortados en la resolución de 200×200 px.
  - Tras modificar texto dinámico con `lv_label_set_text()`, recalcular la posición de elementos anclados a la derecha usando `lv_obj_align(label, LV_ALIGN_TOP_RIGHT, x, y)` para evitar desbordamientos o recortes en el borde derecho.

---

## 3. Patrón de Red y Deep Sleep para Dispositivos IoT

- **Sincronización antes de consultar APIs**:
  - Tras conectar a Wi-Fi, esperar la primera sincronización SNTP (`esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED`) para asegurar que las tablas ARP y NAT del router están operativas antes de realizar peticiones HTTPS TLS.
- **Secuencia de Entrada en Deep Sleep**:
  1. Desconectar y apagar la radio Wi-Fi (`esp_wifi_disconnect()`, `esp_wifi_stop()`).
  2. Poner la pantalla en sleep y apagar su pin de energía (`epaper_port_sleep()`, `GPIO 6 = 1`).
  3. Enclavar el pin de batería (`gpio_hold_en(GPIO_NUM_17)`, `gpio_deep_sleep_hold_en()`).
  4. Configurar temporizador (`esp_sleep_enable_timer_wakeup`) y botón BOOT (`esp_sleep_enable_ext0_wakeup`).
  5. Ejecutar `esp_deep_sleep_start()`.
