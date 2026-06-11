# Waveshare ESP32-S3-ePaper-1.54G

[中文](README_ZH.md)

The ESP32-S3-ePaper-1.54G is an e-paper AIoT development board built around the ESP32-S3-PICO-1-N8R8, featuring a dual-core Xtensa LX7 processor running at up to 240 MHz, 8 MB Flash, 8 MB PSRAM, 2.4 GHz Wi-Fi, and Bluetooth 5 (LE). It integrates a 1.54-inch 200 x 200 four-color e-paper display (red, yellow, black, and white), an ES8311 audio codec, an onboard microphone and speaker, an SHTC3 temperature and humidity sensor, a PCF85063 RTC, a Micro SD card slot, USB Type-C, and a lithium battery interface. The board is suitable for low-power information displays, environmental monitoring, voice interaction, and other IoT applications.

- [Product Documentation](https://docs.waveshare.com/ESP32-S3-ePaper-1.54G/)
- [GitHub Repository](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54G)

<img src="https://www.waveshare.net/photo/LCD/ESP32-S3-ePaper-1.54G/ESP32-S3-ePaper-1.54G-1.jpg" alt="Waveshare ESP32-S3-ePaper-1.54G" width="500">

## Repository Structure

This repository provides sample programs, factory firmware, and hardware design files for the ESP32-S3-ePaper-1.54G.

```text
.
|-- Example/
|   |-- Arduino_3.2.0/    # Arduino examples and libraries
|   |-- ESP-IDF_5.5.1/    # ESP-IDF peripheral and e-paper examples
|   `-- XiaoZhi/          # XiaoZhi AI voice example and SD card resources
|-- Firmware/             # Pre-built factory firmware (.bin)
|-- Hardware/             # Hardware schematic (PDF)
|-- README.md             # English documentation
`-- README_ZH.md          # Chinese documentation
```

## Onboard Resources

- ESP32-S3-PICO-1-N8R8 with 8 MB Flash and 8 MB PSRAM
- 1.54-inch 200 x 200 SPI e-paper display with red, yellow, black, and white colors
- ES8311 audio codec, onboard microphone, speaker, and external speaker connector
- SHTC3 temperature and humidity sensor
- PCF85063 real-time clock
- Micro SD card slot
- USB Type-C port for firmware download and serial logging
- Lithium battery connector, charging circuit, battery voltage measurement, and power button
- 2 x 6-pin 2.54 mm expansion header

## Examples

The [`Example/Arduino_3.2.0`](Example/Arduino_3.2.0) and [`Example/ESP-IDF_5.5.1`](Example/ESP-IDF_5.5.1) directories include examples for ADC battery measurement, the PCF85063 RTC, the SHTC3 sensor, Micro SD, Wi-Fi AP/STA, audio, power control, and the e-paper display.

The [`Example/XiaoZhi`](Example/XiaoZhi) directory contains the XiaoZhi AI voice application and its SD card resources.

## Getting Started

A pre-built test firmware is available in [`Firmware/`](Firmware). For development environment setup, firmware flashing, pin definitions, example usage, and product specifications, refer to the [product documentation](https://docs.waveshare.com/ESP32-S3-ePaper-1.54G/).

## Issues and Support

Open an [issue](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54G/issues) with detailed information, or contact the Waveshare team with your order number for technical support.

---

Thank you for using Waveshare Electronics products!
