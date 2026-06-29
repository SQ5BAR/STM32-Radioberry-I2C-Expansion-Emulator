# STM32 Radioberry I²C Expansion Emulator

STM32F411-based emulator of the I²C peripherals expected by Radioberry-compatible SDR software.

The project emulates the three I²C devices used by the original Radioberry hardware:

- MAX11613 ADC (forward power, reflected power and SWR measurements)
- MCP23008 GPIO expander (filter, relay and open collector control)
- MCP4662 digital potentiometer (power amplifier bias control)

allowing Radioberry-compatible software to operate without the original hardware.

Target MCU:

STM32F411CEU6 Blackpill

---

# Emulated Devices

## MAX11613 (Power / SWR Measurement)

Address: 0x34 (I2C2)

ADC inputs:
- CH0 = PA0
- CH1 = PA1
- CH2 = PA2
- CH3 = PA3

0 ... 3.3 V, 12-bit resolution

Typical signal sources:
- directional couplers
- SWR bridges
- RF power detectors
- analog sensors
- ATU-100 / N7DDC ATU measurement bridges

---

## MCP23008 (Band Filters / Open Collector Control)

Address: 0x20 (I2C1)

GPIO mapping:

- GP0 -> PA4
- GP1 -> PB12
- GP2 -> PB13
- GP3 -> PB14
- GP4 -> PB15
- GP5 -> PB3
- GP6 -> PB5
- GP7 -> PB8

Used for filters, relays, PA switching and open-collector control.

---

## MCP4662 (PA Bias Control)

Address: 0x2C (I2C3)

PWM outputs:

- PA9
- PA10

Recommended bias chain:

PWM -> RC filter -> op-amp buffer -> PA bias circuit

---

# I²C Buses

The STM32F411 Blackpill provides three hardware I²C controllers:

- I2C1
- I2C2
- I2C3

All three emulated devices use unique I²C addresses and may share a single physical SDA/SCL bus.

The separate STM32 I²C peripherals are used internally by the emulator. Externally, all three devices can be connected and used simultaneously exactly like the original Radioberry hardware.

The STM32F411 Blackpill was chosen because it provides three independent hardware I²C peripherals.

---

# Prebuilt Firmware

Included files:

- build/stm32_radioberry_i2c_emulator_3in1.bin
- build/stm32_radioberry_i2c_emulator_3in1.hex
- build/stm32_radioberry_i2c_emulator_3in1.elf

---

# Programming via USB (Recommended)

Install:

sudo apt update
sudo apt install dfu-util

Enter DFU mode:

1. BOOT0 = 1
2. RESET
3. Connect USB

Check:

dfu-util -l

Flash:

dfu-util -a 0 -s 0x08000000:leave -D build/stm32_radioberry_i2c_emulator_3in1.bin

Run:

1. BOOT0 = 0
2. RESET

---

# Building From Source

Open in STM32CubeIDE:

File → Open Projects from File System...

Target MCU:

STM32F411CEU6
