# STM32 Radioberry I²C Expansion Emulator

STM32F411-based emulator of common I²C expansion peripherals used with the Radioberry SDR platform.

> This project emulates three external I²C peripherals commonly used with Radioberry-based SDR setups using a single STM32F411 Blackpill board.

The project emulates the following I²C devices commonly used with Radioberry-based hardware:

* MAX11613 ADC for forward power, reflected power and SWR measurements
* MCP23008 GPIO expander for filter, relay and open collector control
* MCP4662 digital potentiometer for power amplifier bias control

This allows Radioberry-compatible software and hardware to use STM32-based emulation of these expansion peripherals instead of separate external ICs.

---

## Target Hardware

* Host platform: Raspberry Pi 4/5 with Radioberry
* Emulator MCU: STM32F411CEU6 Blackpill

---

## Hardware Connection Overview

<img width="733" height="803" alt="STM32 Radioberry I2C Expansion Emulator wiring diagram" src="https://github.com/user-attachments/assets/86f66810-2a83-4e94-8be7-6a729e7ab321" />

---

## Wiring Summary

### Raspberry Pi / Radioberry I²C Bus

```text
Raspberry Pi Pin 3 / GPIO2 / SDA
 ├── PB9  (MAX11613 SDA)
 ├── PB7  (MCP23008 SDA)
 └── PB4  (MCP4662 SDA)

Raspberry Pi Pin 5 / GPIO3 / SCL
 ├── PB10 (MAX11613 SCL)
 ├── PB6  (MCP23008 SCL)
 └── PA8  (MCP4662 SCL)

Raspberry Pi Pin 1 / 3.3V → Blackpill 3.3V / VCC
Raspberry Pi Pin 6 / GND  → Blackpill GND
```

> The I²C bus uses 3.3 V logic only.
>
> Do not connect 5 V logic to the STM32 pins.
>
> A common GND between Raspberry Pi / Radioberry and Blackpill is required.

If the Blackpill is powered from USB, the 3.3 V connection from Raspberry Pi is not required, but the common GND connection is still required.

---

# Emulated Devices

## MAX11613 — Power / SWR Measurement

**I²C address:** `0x34`

**STM32 I²C bus:**

```text
PB10 (SCL)
PB9  (SDA)
```

The original MAX11613 ADC is used with Radioberry-based setups for:

* forward power measurement
* reflected power measurement
* SWR calculation
* analog telemetry inputs

The emulator uses the STM32 ADC inputs:

```text
CH0 (PA0 / ADC1_IN0)
CH1 (PA1 / ADC1_IN1)
CH2 (PA2 / ADC1_IN2)
CH3 (PA3 / ADC1_IN3)
```

### Specifications

* 0 ... 3.3 V input range
* 12-bit ADC resolution

### Typical Signal Sources

* directional couplers
* SWR bridges
* RF power detectors
* analog sensors
* ATU-100 / N7DDC-style measurement bridges

The analog signals connected to PA0 ... PA3 must be scaled to the STM32 input range:

```text
0 ... 3.3 V
```

Do not exceed 3.3 V on any STM32 ADC input.

---

## MCP23008 — Band Filters / Relay / Open Collector Control

**I²C address:** `0x20`

**STM32 I²C bus:**

```text
PB6 (SCL)
PB7 (SDA)
```

In Radioberry-based designs the MCP23008 GPIO expander can be used to control:

* band-pass filters
* low-pass filters
* antenna relays
* open collector outputs
* power amplifier switching
* external control logic

### GPIO Mapping

```text
GP0 (PA4)
GP1 (PB12)
GP2 (PB13)
GP3 (PB14)
GP4 (PB15)
GP5 (PB3)
GP6 (PB5)
GP7 (PB8)
```

The outputs can be connected to relay drivers, transistor stages or other control circuitry.

Use suitable driver stages for relays or other loads. Do not drive relay coils directly from STM32 pins.

---

## MCP4662 — PA Bias Control

**I²C address:** `0x2C`

**STM32 I²C bus:**

```text
PA8 (SCL)
PB4 (SDA)
```

The MCP4662 digital potentiometer is commonly used for PA bias control.

The emulator converts the requested digital potentiometer setting into PWM outputs:

```text
PWM A (PA9  / TIM1_CH2 PWM)
PWM B (PA10 / TIM1_CH3 PWM)
```

### Recommended Bias Circuit

The PWM signal should be converted into a stable analog voltage before being used as a PA bias source.

```text
PWM output
   ↓
RC low-pass filter
   ↓
Operational amplifier buffer
   ↓
PA bias circuit
```

Direct connection of the PWM signal to a PA bias network is not recommended.

---

# I²C Architecture

Three independent STM32 I²C peripherals are used internally:

```text
MAX11613  0x34  I2C2
MCP23008  0x20  I2C1
MCP4662   0x2C  I2C3
```

The STM32F411 Blackpill was selected because the STM32F411 provides three independent hardware I²C controllers.

Although the firmware internally emulates the devices on separate STM32 I²C peripherals, all three buses can be connected externally to one shared physical I²C bus.

This is possible because each emulated device uses a unique I²C address:

```text
0x20  MCP23008
0x2C  MCP4662
0x34  MAX11613
```

From the Raspberry Pi / Radioberry side, the emulator behaves like three different I²C devices connected to the same SDA and SCL lines.

---

# Typical Use Case

```text
Raspberry Pi + Radioberry
          │
          │ I²C
          ▼
 STM32F411 Blackpill Emulator
 ├── MAX11613 → SWR and power measurement inputs
 ├── MCP23008 → filter, relay and open collector outputs
 └── MCP4662  → PA bias PWM outputs
```

External hardware may include:

* directional coupler
* ATU-100 / N7DDC measurement bridge
* band-pass filters
* low-pass filters
* relay boards
* power amplifier
* custom RF hardware

---

# Status LED

```text
PC13 (Activity LED)
```

The PC13 LED briefly blinks after successful I²C activity.

---

# Prebuilt Firmware

Ready-to-use firmware files are included in the `build` directory:

```text
build/stm32_radioberry_i2c_emulator_3in1.bin
```

For most users, no compilation is required.

---

# Programming via USB DFU — Recommended

The STM32F411 Blackpill contains a factory USB DFU bootloader and can be programmed directly from Linux without an ST-Link programmer.

## Install DFU Utility

```bash
sudo apt update
sudo apt install dfu-util
```

## Enter DFU Mode

1. Set `BOOT0 = 1`
2. Press `RESET`
3. Connect USB

Verify detection:

```bash
dfu-util -l
```

## Flash Firmware

```bash
dfu-util -a 0 -s 0x08000000:leave \
  -D build/stm32_radioberry_i2c_emulator_3in1.bin
```

## Run Firmware

1. Set `BOOT0 = 0`
2. Press `RESET`

---

# Programming via UART — Alternative

Install `stm32flash`:

```bash
sudo apt install stm32flash
```

UART bootloader wiring:

```text
Blackpill PA9  (TX1) → USB-UART RX
Blackpill PA10 (RX1) → USB-UART TX
Blackpill GND        → USB-UART GND
```

Flash command:

```bash
stm32flash \
  -w build/stm32_radioberry_i2c_emulator_3in1.bin \
  -v \
  -g 0x0 \
  /dev/ttyUSB0
```

> PA9 and PA10 are used for UART bootloader programming only when the STM32 is in bootloader mode.
>
> During normal firmware operation, PA9 and PA10 are used as PWM outputs for the MCP4662 emulator.

---

# Building From Source

Open the project in STM32CubeIDE:

```text
File → Open Projects from File System...
```

or:

```text
File → Import...
      → General
      → Existing Projects into Workspace
```

Target MCU:

```text
STM32F411CEU6
```

The main application logic is located in:

```text
Core/Src/main.c
```

---

# Tested Software

- piHPSDR on Raspberry Pi / Radioberry
- piHPSDR on Linux
- Thetis on Windows
- SparkSDR

---

# Tested Hardware

* Raspberry Pi 4 / 5 with Radioberry
* STM32F411CEU6 Blackpill

---
