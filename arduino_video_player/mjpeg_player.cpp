#include "mjpeg_player.h"
#include "config.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>
#include <dirent.h>
#include <algorithm>
#include <esp_timer.h>

#define TAG "MjpegPlayer"

// ── Static members shared with JPEGDEC draw callback ────────────────
int          MjpegPlayer::offset_x_   = 0;
int          MjpegPlayer::offset_y_   = 0;
Arduino_GFX* MjpegPlayer::static_gfx_ = nullptr;

// ── Construction / Destruction ──────────────────────────────────────

MjpegPlayer::MjpegPlayer() {}

MjpegPlayer::~MjpegPlayer() {
    stop();
    unmountSdCard();
}

// ── Display (Arduino_GFX) ───────────────────────────────────────────

bool MjpegPlayer::initDisplay() {
    if (gfx_) return true;

    ESP_LOGI(TAG, "Creating Arduino_GFX display (ESP32SPI DMA)...");

    // Arduino_ESP32SPI: DMA-backed SPI bus driver
    // is_shared_interface = true  →  releases bus between operations
    //                                so the SD card can share SPI3_HOST
    bus_ = new Arduino_ESP32SPI(
        DISPLAY_DC_PIN, DISPLAY_CS_PIN,
        DISPLAY_CLK_PIN, DISPLAY_MOSI_PIN, DISPLAY_MISO_PIN,
        SPI3_HOST,   // same host as SD card
        true         // shared interface
    );

    // ST7789 display driver
    // ips = true : for IPS panels (most ST7789 modules).
    //              If colors look inverted, change to false.
    gfx_ = new Arduino_ST7789(
        bus_,
        DISPLAY_RST_PIN,     // reset (-1 = none)
        0,                   // rotation
        true,                // IPS
        DISPLAY_WIDTH, DISPLAY_HEIGHT,
        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y
    );

    gfx_->begin();
    gfx_->fillScreen(BLACK);

    static_gfx_ = gfx_;
    ESP_LOGI(TAG, "Display ready (%dx%d, DMA, shared SPI bus).",
             DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return true;
}

// ── SD Card (ESP-IDF SDSPI, shared SPI bus) ─────────────────────────

bool MjpegPlayer::mountSdCard() {
    if (sd_mounted_) return true;

    ESP_LOGI(TAG, "Mounting SD card (SDSPI on SPI3_HOST)...");

    sdspi_device_config_t slot_cfg = {};
    slot_cfg.host_id  = SD_SPI_HOST;
    slot_cfg.gpio_cs  = (gpio_num_t)SD_CARD_CS_PIN;
    slot_cfg.gpio_cd  = SDSPI_SLOT_NO_CD;
    slot_cfg.gpio_wp  = GPIO_NUM_NC;
    slot_cfg.gpio_int = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files              = 5;
    mount_cfg.allocation_unit_size   = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    sdmmc_host_t  sdhost = SDSPI_HOST_DEFAULT();
    sdhost.slot = SD_SPI_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT, &sdhost, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    sd_mounted_ = true;
    return true;
}

void MjpegPlayer::unmountSdCard() {
    if (!sd_mounted_) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, nullptr);
    sd_mounted_ = false;
    ESP_LOGI(TAG, "SD card unmounted.");
}

// ── Utility ─────────────────────────────────────────────────────────

void MjpegPlayer::clearScreen(uint16_t color) {
    if (gfx_) gfx_->fillScreen(color);
}

std::string MjpegPlayer::getCurrentFileName() const {
    if (current_file_index_ >= 0 &&
        current_file_index_ < (int)video_files_.size()) {
        const std::string& full = video_files_[current_file_index_];
        size_t pos = full.find_last_of('/');
        return (pos != std::string::npos) ? full.substr(pos + 1) : full;
    }
    return "";
}

// ── File scanning ───────────────────────────────────────────────────

void MjpegPlayer::scanVideoFiles(const char* dir) {
    video_files_.clear();
    DIR* d = opendir(dir);
    if (!d) { ESP_LOGE(TAG, "opendir(%s) failed", dir); return; }

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_type == DT_DIR) continue;

        std::string name(entry->d_name);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.size() > 6 &&
            lower.substr(lower.size() - 6) == ".mjpeg") {
            std::string path = std::string(dir) + "/" + name;
            video_files_.push_back(path);
            ESP_LOGI(TAG, "  + %s", path.c_str());
        }
    }
    closedir(d);
    std::sort(video_files_.begin(), video_files_.end());
    ESP_LOGI(TAG, "Found %d video(s).", (int)video_files_.size());
}

// ── Playback control ────────────────────────────────────────────────

bool MjpegPlayer::start() {
    if (playing_.load()) { ESP_LOGW(TAG, "Already playing."); return false; }
    if (!sd_mounted_)    { ESP_LOGE(TAG, "SD not mounted.");  return false; }

    scanVideoFiles(VIDEO_DIR);
    if (video_files_.empty()) {
        ESP_LOGW(TAG, "No .mjpeg files found.");
        return false;
    }

    current_file_index_ = 0;
    stop_requested_.store(false);
    skip_requested_.store(false);
    playing_.store(true);
    paused_.store(false);

    xTaskCreatePinnedToCore(
        [](void* arg) {
            static_cast<MjpegPlayer*>(arg)->playbackLoop();
            vTaskDelete(nullptr);
        },
        "mjpeg_play", 32768, this, 5, &playback_task_, 1);

    ESP_LOGI(TAG, "Playback started.");
    return true;
}

void MjpegPlayer::stop() {
    if (!playing_.load()) return;
    stop_requested_.store(true);
    if (playback_task_) {
        for (int i = 0; i < 50 && playing_.load(); i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        playback_task_ = nullptr;
    }
    ESP_LOGI(TAG, "Playback stopped.");
}

void MjpegPlayer::next()        { skip_requested_.store(true); }
void MjpegPlayer::togglePause() {
    paused_.store(!paused_.load());
    ESP_LOGI(TAG, "Playback %s.", paused_.load() ? "PAUSED" : "RESUMED");
}

// ── Playback loop (runs on Core 1) ─────────────────────────────────

void MjpegPlayer::playbackLoop() {
    while (!stop_requested_.load() &&
           current_file_index_ < (int)video_files_.size()) {

        const std::string& path = video_files_[current_file_index_];
        ESP_LOGI(TAG, "Playing [%d/%d]: %s",
                 current_file_index_ + 1, (int)video_files_.size(),
                 path.c_str());

        skip_requested_.store(false);
        playSingleVideo(path);

        if (!stop_requested_.load()) {
            current_file_index_++;
            if (current_file_index_ >= (int)video_files_.size())
                current_file_index_ = 0;   // loop playlist
        }
    }
    playing_.store(false);
}

bool MjpegPlayer::playSingleVideo(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) { ESP_LOGE(TAG, "fopen(%s) failed", path.c_str()); return false; }

    // Prefer PSRAM for the large frame buffer
    uint8_t* mjpeg_buf = (uint8_t*)heap_caps_malloc(
        MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mjpeg_buf) mjpeg_buf = (uint8_t*)malloc(MAX_JPEG_SIZE);
    uint8_t* read_buf = (uint8_t*)malloc(READ_BUFFER_SIZE);

    if (!mjpeg_buf || !read_buf) {
        ESP_LOGE(TAG, "Buffer alloc failed.");
        free(mjpeg_buf); free(read_buf); fclose(fp);
        return false;
    }

    int buf_offset = 0, buf_len = 0, frame_count = 0;
    int64_t start_us = esp_timer_get_time();

    while (!stop_requested_.load() && !skip_requested_.load()) {

        // ── Pause ──
        while (paused_.load() && !stop_requested_.load() &&
               !skip_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            start_us = esp_timer_get_time() -
                       ((int64_t)frame_count * 1000000 / VIDEO_FPS);
        }

        // ── Read next JPEG frame ──
        int frame_size = 0;
        if (!readNextJpegFrame(fp, mjpeg_buf, &frame_size,
                               read_buf, &buf_offset, &buf_len))
            break;   // EOF

        // ── Frame-rate control ──
        int64_t target_us =
            start_us + ((int64_t)(frame_count + 1) * 1000000 / VIDEO_FPS);
        int64_t now_us = esp_timer_get_time();

        if (now_us < target_us) {
            decodeAndDisplay(mjpeg_buf, frame_size);
            now_us = esp_timer_get_time();
            if (now_us < target_us)
                vTaskDelay(pdMS_TO_TICKS((target_us - now_us) / 1000));
        } else {
            ESP_LOGD(TAG, "Frame skip (%lld ms behind)",
                     (now_us - target_us) / 1000);
        }
        frame_count++;
    }

    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG, "Finished: %d frames in %lld ms (%.1f fps).",
             frame_count, elapsed_ms,
             elapsed_ms > 0 ? (frame_count * 1000.0 / elapsed_ms) : 0.0);

    free(mjpeg_buf); free(read_buf); fclose(fp);
    return true;
}

// ── MJPEG frame extraction (SOI / EOI marker scan) ─────────────────

bool MjpegPlayer::readNextJpegFrame(
        FILE* fp, uint8_t* mjpeg_buf, int* frame_size,
        uint8_t* read_buf, int* buf_offset, int* buf_len) {

    *frame_size = 0;

    if (*buf_len <= 0) {
        *buf_len = fread(read_buf, 1, READ_BUFFER_SIZE, fp);
        *buf_offset = 0;
        if (*buf_len <= 0) return false;
    }

    // Find FFD8 (SOI)
    bool found_soi = false;
    while (!found_soi) {
        for (int i = *buf_offset; i < *buf_len - 1; i++) {
            if (read_buf[i] == 0xFF && read_buf[i + 1] == 0xD8) {
                *buf_offset = i;
                found_soi = true;
                break;
            }
        }
        if (!found_soi) {
            if (*buf_len > 0 && read_buf[*buf_len - 1] == 0xFF) {
                read_buf[0] = 0xFF;
                *buf_len = fread(read_buf + 1, 1, READ_BUFFER_SIZE - 1, fp);
                if (*buf_len <= 0) return false;
                *buf_len += 1;
            } else {
                *buf_len = fread(read_buf, 1, READ_BUFFER_SIZE, fp);
                if (*buf_len <= 0) return false;
            }
            *buf_offset = 0;
        }
    }

    // Copy until FFD9 (EOI)
    int out = 0;
    bool found_eoi = false;
    while (!found_eoi) {
        int rem = *buf_len - *buf_offset;
        for (int i = 0; i < rem - 1; i++) {
            int si = *buf_offset + i;
            if (read_buf[si] == 0xFF && read_buf[si + 1] == 0xD9) {
                int n = i + 2;
                if (out + n > MAX_JPEG_SIZE) return false;
                memcpy(mjpeg_buf + out, read_buf + *buf_offset, n);
                out += n;
                *buf_offset += n;
                found_eoi = true;
                break;
            }
        }
        if (!found_eoi) {
            int n = rem - 1;
            if (n > 0) {
                if (out + n > MAX_JPEG_SIZE) return false;
                memcpy(mjpeg_buf + out, read_buf + *buf_offset, n);
                out += n;
            }
            read_buf[0] = read_buf[*buf_len - 1];
            *buf_len = fread(read_buf + 1, 1, READ_BUFFER_SIZE - 1, fp);
            if (*buf_len <= 0) return false;
            *buf_len += 1;
            *buf_offset = 0;
        }
    }

    *frame_size = out;
    return out > 0;
}

// ── JPEG decode + display (JPEGDEC → Arduino_GFX) ──────────────────

bool MjpegPlayer::decodeAndDisplay(const uint8_t* jpeg_data, int jpeg_size) {
    JPEGDEC* jpeg = new (std::nothrow) JPEGDEC();
    if (!jpeg) return false;

    bool ok = false;
    if (jpeg->openRAM((uint8_t*)jpeg_data, jpeg_size, drawCallback)) {
        int w = jpeg->getWidth();
        int h = jpeg->getHeight();
        offset_x_ = (DISPLAY_WIDTH  - w) / 2;
        offset_y_ = (DISPLAY_HEIGHT - h) / 2;
        if (offset_x_ < 0) offset_x_ = 0;
        if (offset_y_ < 0) offset_y_ = 0;

        // Output big-endian RGB565 — matches ST7789 byte order.
        // Arduino_GFX draw16bitBeRGBBitmap() sends it straight
        // to SPI DMA with zero conversion.
        jpeg->setPixelType(RGB565_BIG_ENDIAN);

        ok = (jpeg->decode(0, 0, 0) == 1);
        if (!ok) ESP_LOGW(TAG, "JPEG decode fail (%dx%d, %d B)", w, h, jpeg_size);
        jpeg->close();
    } else {
        ESP_LOGW(TAG, "JPEG openRAM fail (%d B)", jpeg_size);
    }
    delete jpeg;
    return ok;
}

/**
 * Called by JPEGDEC for each decoded MCU block.
 * Pixels are already big-endian RGB565  →  direct DMA to display.
 */
int MjpegPlayer::drawCallback(JPEGDRAW *pDraw) {
    if (!static_gfx_) return 0;

    static_gfx_->draw16bitBeRGBBitmap(
        offset_x_ + pDraw->x,
        offset_y_ + pDraw->y,
        pDraw->pPixels,
        pDraw->iWidth,
        pDraw->iHeight);

    return 1;   // continue decoding
}
