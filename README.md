# ESP32 WAV Player + Real-Time FFT Visualizer

A WAV music player for the ESP32 with a real-time FFT spectrum analyzer
displayed on a 128×64 OLED screen. Audio is read from a MicroSD card and
output through a MAX98357A I2S amplifier to a 3.5mm headphone jack.

---

## Features

- WAV playback from MicroSD card (FAT32, root directory)
- Real-time 32-bar FFT spectrum analyzer on OLED (20 FPS)
- Peak hold markers with gravity decay on spectrum display
- Play / Pause, Skip Forward / Backward, Volume Up / Down
- Auto-advances to next track on playback completion
- Non-blocking architecture — no `delay()` in main loop
- Software debounced active-low buttons (200ms refractory window)

---

## Hardware

| Component | Model |
|---|---|
| Microcontroller | ESP32 ESP-WROOM-32 Dev Board |
| MicroSD Module | WWZMDiB SPI MicroSD Breakout |
| DAC / Amplifier | MAX98357A I2S Class-D Amplifier |
| Display | UCTRONICS 0.96" 128×64 I2C OLED (SSD1306) |
| Audio Output | 3.5mm TRS Headphone Jack Breakout |
| Buttons | 5× Tactile Pushbutton (Active-Low) |

---

## Wiring

### MicroSD Card — SPI
| Module Pin | ESP32 GPIO |
|---|---|
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| SS (CS) | GPIO 5 |
| VCC | 3.3V |
| GND | GND |

### MAX98357A — I2S
| Module Pin | ESP32 GPIO |
|---|---|
| BCLK | GPIO 26 |
| LRC | GPIO 25 |
| DIN | GPIO 22 |
| Vin | 5V |
| GND | GND |

### OLED Display — I2C
| Module Pin | ESP32 GPIO |
|---|---|
| SCL | GPIO 27 |
| SDA | GPIO 21 |
| VCC | 3.3V |
| GND | GND |

> **Note:** SCL is remapped to GPIO 27 to avoid conflict with I2S DIN on GPIO 22.
> The ESP32 routing matrix allows I2C to be assigned to any GPIO pin.

### Buttons — Active-Low
| Function | ESP32 GPIO |
|---|---|
| Play / Pause | GPIO 13 |
| Skip Forward | GPIO 14 |
| Skip Backward | GPIO 4 |
| Volume Up | GPIO 32 |
| Volume Down | GPIO 33 |

> Each button connects between its GPIO pin and GND.
> Internal `INPUT_PULLUP` resistors (~45kΩ) eliminate the need for external resistors.

### 3.5mm Headphone Jack
| Jack Pin | Connect To |
|---|---|
| Tip (Left) | MAX98357A Speaker (+) |
| Ring (Right) | MAX98357A Speaker (+) |
| Sleeve (GND) | MAX98357A Speaker (−) |

> **Warning:** The MAX98357A uses Bridge-Tied Load (BTL) topology.
> Speaker (−) is NOT ground. Do not connect it to ESP32 GND.

---

## Software Requirements

### Arduino IDE Board Setup
1. Open **File → Preferences**
2. Add to Additional Board Manager URLs:
