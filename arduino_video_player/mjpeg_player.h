#ifndef _MJPEG_PLAYER_H_
#define _MJPEG_PLAYER_H_

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>
#include <atomic>

/**
 * @brief Standalone MJPEG video player using Arduino_GFX + JPEGDEC.
 *
 * Uses Arduino_ESP32SPI (DMA-backed) for the display and ESP-IDF SDSPI
 * for the SD card, both sharing SPI3_HOST.
 *
 * JPEGDEC outputs big-endian RGB565 directly into Arduino_GFX's
 * draw16bitBeRGBBitmap() — zero byte-swap, zero memcpy.
 */
class MjpegPlayer {
public:
    MjpegPlayer();
    ~MjpegPlayer();

    /** Initialize SPI bus + ST7789 display via Arduino_GFX. */
    bool initDisplay();

    /** Mount SD card via ESP-IDF SDSPI (bus already initialized by display). */
    bool mountSdCard();
    void unmountSdCard();
    bool isSdMounted() const { return sd_mounted_; }

    /** Fill screen with a solid RGB565 color. */
    void clearScreen(uint16_t color);

    /** Scan for .mjpeg files and begin looping playback. */
    bool start();
    void stop();
    void next();
    void togglePause();

    bool isPlaying() const { return playing_.load(); }
    bool isPaused()  const { return paused_.load(); }
    size_t getVideoCount()  const { return video_files_.size(); }
    int    getCurrentIndex() const { return current_file_index_; }
    std::string getCurrentFileName() const;

    /** Expose the GFX handle for external use (e.g. text overlay). */
    Arduino_GFX* getGfx() const { return gfx_; }

    /** JPEGDEC draw callback — called once per MCU block. */
    static int drawCallback(JPEGDRAW *pDraw);

private:
    // Display (Arduino_GFX)
    Arduino_DataBus* bus_ = nullptr;
    Arduino_GFX*     gfx_ = nullptr;

    // SD card
    bool sd_mounted_ = false;

    // Playback state
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> skip_requested_{false};
    TaskHandle_t playback_task_ = nullptr;

    // Video playlist
    std::vector<std::string> video_files_;
    int current_file_index_ = 0;

    // Buffer limits
    static constexpr int READ_BUFFER_SIZE = 1024;
    static constexpr int MAX_JPEG_SIZE    = 120 * 1024;

    // Statics shared with the JPEGDEC draw callback
    static Arduino_GFX* static_gfx_;
    static int offset_x_;
    static int offset_y_;

    // Internal helpers
    void scanVideoFiles(const char* dir);
    void playbackLoop();
    bool playSingleVideo(const std::string& path);
    bool readNextJpegFrame(FILE* fp, uint8_t* mjpeg_buf, int* frame_size,
                           uint8_t* read_buf, int* buf_offset, int* buf_len);
    bool decodeAndDisplay(const uint8_t* jpeg_data, int jpeg_size);
};

#endif // _MJPEG_PLAYER_H_
