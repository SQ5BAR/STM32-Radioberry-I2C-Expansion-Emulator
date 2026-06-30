# STM32 Radioberry I²C Expansion Emulator

STM32F411-based emulator of the I²C peripherals expected by Radioberry-compatible SDR software.

> This project emulates the three external I²C peripherals used by Radioberry SDR hardware using a single STM32F411 Blackpill board.
>
> No original Radioberry hardware is required.

The project emulates the three I²C devices used by the original Radioberry hardware:

- MAX11613 ADC (forward power, reflected power and SWR measurements)
- MCP23008 GPIO expander (filter, relay and open collector control)
- MCP4662 digital potentiometer (power amplifier bias control)

allowing Radioberry-compatible software to operate without the original hardware.

## Target MCU

- STM32F411CEU6 Blackpill

---
<img width="606" height="660" alt="obraz" src="https://github.com/user-attachments/assets/de84458f-a443-4316-afe6-8d3d239b652a" />

# Emulated Devices

## MAX11613 (Power / SWR Measurement)

**I²C Address:** `0x34`

**Bus:**

- I2C2
- PB10 = SCL
- PB9 = SDA

The original MAX11613 ADC is used by Radioberry for:

- forward power measurement
- reflected power measurement
- SWR calculation
- analog telemetry inputs

The emulator uses the STM32 ADC inputs:

| ADC Channel | STM32 Pin |
|------------|------------|
| CH0 | PA0 |
| CH1 | PA1 |
| CH2 | PA2 |
| CH3 | PA3 |

### Specifications

- 0 ... 3.3 V input range
- 12-bit resolution

### Typical signal sources

- directional couplers
- SWR bridges
- RF power detectors
- analog sensors

A practical source of forward/reflected power signals is the measurement bridge used in projects such as:

- ATU-100
- N7DDC ATU
- similar automatic antenna tuners

The bridge outputs should be scaled to **0 ... 3.3 V** before connection to the STM32 ADC inputs.

---

## MCP23008 (Band Filters / Open Collector Control)

**I²C Address:** `0x20`

**Bus:**

- I2C1
- PB6 = SCL
- PB7 = SDA

In the original Radioberry design the MCP23008 GPIO expander is used to control:

- band-pass filters
- low-pass filters
- antenna relays
- open-collector outputs
- power amplifier switching

### GPIO Mapping

| MCP23008 Pin | STM32 Pin |
|-------------|------------|
| GP0 | PA4 |
| GP1 | PB12 |
| GP2 | PB13 |
| GP3 | PB14 |
| GP4 | PB15 |
| GP5 | PB3 |
| GP6 | PB5 |
| GP7 | PB8 |

The outputs can be connected directly to relay drivers, transistor stages or other control circuitry.

---

## MCP4662 (PA Bias Control)

**I²C Address:** `0x2C`

**Bus:**

- I2C3
- PA8 = SCL
- PB4 = SDA

The original Radioberry uses the MCP4662 digital potentiometer to control power amplifier bias voltage.

The emulator converts the requested digital potentiometer setting into PWM outputs:

| Function | STM32 Pin |
|----------|------------|
| PWM A | PA9 |
| PWM B | PA10 |

### Recommended Bias Circuit

The PWM signal should be converted into a stable analog voltage before being used as a PA bias source.

```text
PWM
  ↓
RC low-pass filter
  ↓
Operational amplifier buffer
  ↓
PA bias circuit
```

Direct connection of the PWM signal to a PA bias network is not recommended.

---

# I²C Buses

Three independent STM32 I²C peripherals are used:

| Device | Address | Bus |
|----------|---------|---------|
| MAX11613 | 0x34 | I2C2 |
| MCP23008 | 0x20 | I2C1 |
| MCP4662 | 0x2C | I2C3 |

The devices are separated across three STM32 I²C controllers because the STM32F411 Blackpill provides three hardware I²C peripherals.

In a practical application the three buses can be connected together and used as a single shared I²C bus because all emulated devices use different I²C addresses:

```text
0x20  MCP23008
0x2C  MCP4662
0x34  MAX11613
```

The firmware internally emulates the devices on separate STM32 I²C peripherals, but from the host perspective they may share the same SDA and SCL lines.

This means all three emulated devices can be used simultaneously on a single physical I²C bus exactly as on the original Radioberry hardware.

The STM32F411 Blackpill was selected primarily because it provides three independent hardware I²C controllers, making it possible to emulate all three devices concurrently while maintaining reliable operation and leaving sufficient processing resources for future expansion.

---

# Typical Use Case

```text
Radioberry Software
        │
        ▼
 STM32F411 Emulator
 ├─ MAX11613 → SWR and power measurements
 ├─ MCP23008 → Filter and relay control
 └─ MCP4662 → PA bias generation
```

External hardware may include:

- directional coupler
- ATU-100 measurement bridge
- band-pass filters
- low-pass filters
- relay boards
- power amplifier
- custom RF hardware

---

# Status LED

| Function | STM32 Pin |
|----------|------------|
| Activity LED | PC13 |

The LED briefly blinks after successful I²C activity.

---

# Features

- MAX11613 emulation
- MCP23008 emulation
- MCP4662 emulation
- ADC sampling via DMA
- PWM generation using TIM1
- I²C bus recovery
- PC13 activity indication
- STM32CubeIDE project

---

# Prebuilt Firmware

Ready-to-use firmware files are included in the `build` directory:

- `build/stm32_radioberry_i2c_emulator_3in1.bin`

For most users no compilation is required.

---

# Programming via USB (Recommended)

The STM32F411 Blackpill contains a factory USB bootloader and can be programmed directly from Linux without an ST-Link programmer.

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

# Programming via UART (Alternative)

```bash
sudo apt install stm32flash
```

| Blackpill | USB-UART |
|------------|----------|
| PA9 (TX1) | RX |
| PA10 (RX1) | TX |
| GND | GND |

```bash
stm32flash \
  -w build/stm32_radioberry_i2c_emulator_3in1.bin \
  -v \
  -g 0x0 \
  /dev/ttyUSB0
```

---

# Building From Source

Open in STM32CubeIDE:

```text
File → Open Projects from File System...
```

or

```text
File → Import...
      → General
      → Existing Projects into Workspace
```

Target MCU:

- STM32F411CEU6

---

# Tested Software

- piHPSDR
- SparkSDR
- Radioberry compatible SDR software

## Tested Hardware

- STM32F411CEU6 Blackpill

---

# License

This project is provided as-is without warranty.
