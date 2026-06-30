# Hardware Specification: ESP32-C3, ESP32-S3, and Raspberry Pi

## 1. System Architecture Overview
The ESP32-S3 communicates with tthe ESP32-C3 over ESP-NOW, using ESP-NOW channel 1. The recieved data from the ESP32-C3 gets sent via serial to the Raspberry Pi

* **Communication Bus:** ESP-NOW
* **Primary Host:** Raspberry Pi 5
* **Remote Controller:** ESP32-S3
* **Receiver Dongle:** ESP32-C3 Super Mini
* **Display:** 2.42" SSD1309 128x64 OLED
* **Display Interface:** 4-wire SPI
* **Button Behavior:** Buttons are wired between the GPIO pin and GND. The firmware uses `INPUT_PULLUP`, so an unpressed button reads `HIGH` and a pressed button reads `LOW`.


---

## 2. Pinout Mappings and Wiring

### ESP32-S3 wiring

| Signal | ESP32 Pin | Connection |
| :--- | :---: | :--- |
| GND | GND | GND |
| 3.3V | 3.3V | OLED |
| Play/Pause | D2 | Switch |
| Next | D3 | Switch |
| Previous | D4 | Switch |
| Volume + | D5 | Switch |
| Volume - | D6 | Switch |
| SCK | D13 | OLED |
| SDA | D11 | OLED |
| RES | D8 | OLED |
| DC | D9 | OLED |
| CS | D10 | OLED |

---

## 3. Power Requirements and Electrical Specs

> [!NOTE]
> As of v1.0.0, both ESP32 boards and the Raspberry Pi are powered over USB-C.

* **GPIO Logic Level:** 3.3V
* **OLED Supply:** 3.3V from ESP32-S3
* **Button Inputs:** Active-low using internal pull-up resistors
