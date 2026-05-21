# Standalone Arduino MJPEG Video Player for DogePet V2

A completely standalone Arduino IDE sketch for playing `.mjpeg` video on the **DogePet V2** board (ESP32-S3). It mounts the SD card, scans for video files, and plays them in an infinite loop using a high-performance rendering pipeline.

## Performance Stack
| Layer | Library | Role |
|---|---|---|
| **Display** | **Arduino_GFX** (`Arduino_ESP32SPI`) | DMA-backed SPI driver for ST7789 — zero-copy pixel transfer |
| **JPEG Decode** | **JPEGDEC** (Larry Bank) | Fast software JPEG decoder, outputs big-endian RGB565 directly |
| **SD Card** | **ESP-IDF SDSPI** | VFS-mounted FAT32, shares SPI3_HOST with display |

JPEGDEC outputs big-endian RGB565 → `draw16bitBeRGBBitmap()` sends pixels straight to SPI DMA. **Zero byte-swap, zero intermediate buffer.**

## Features
- **Background playback** on FreeRTOS Core 1 with precise frame-rate control
- **Button A (GPIO 39)**: Pause / Resume
- **Button B (GPIO 40)**: Skip to next video
- **BOOT button (GPIO 0)**: Print diagnostics (file name, heap, playlist size)
- **Visual state indicators**: Color-coded screen fills for startup / pause / error states
- **Playlist looping**: Plays all `.mjpeg` files alphabetically, loops forever

---

## 🛠️ Arduino IDE Setup

### 1. Board Selection
1. **Tools > Board > esp32 > ESP32S3 Dev Module** (or your specific S3 board variant)
2. **USB CDC On Boot**: *Enabled*
3. **PSRAM**: *Enabled* (recommended for large frame buffers)
4. **Partition Scheme**: *Huge App (3MB No OTA)* or *Default 4MB with spiffs*

### 2. Install Libraries
Open **Sketch > Include Library > Manage Libraries** and install:
1. **GFX Library for Arduino** (by Moon On Our Nation) — display driver
2. **JPEGDEC** (by Larry Bank) — JPEG decoder

---

## 📂 Preparing the SD Card

1. Format your microSD card to **FAT32**
2. Convert videos using `ffmpeg`:
   ```bash
   ffmpeg -i input.mp4 -vf "scale=240:320,fps=15" -q:v 9 -an output.mjpeg
   ```
   Adjust `240:320` to match your display config in `config.h`.
3. Copy `.mjpeg` files to the **root** of the SD card
4. Insert and boot!

## 🎨 Display Configuration
Edit `config.h` to select your screen size (uncomment **one**):
```c
#define CONFIG_LCD_ST7789_240X320   // 2.0" / 2.4" tall (default)
// #define CONFIG_LCD_ST7789_240X280 // 1.69" display
// #define CONFIG_LCD_ST7789_240X240 // 1.3" square
```
If colors look inverted, change the `ips` parameter in `mjpeg_player.cpp` `initDisplay()` from `true` to `false` (or vice versa).
