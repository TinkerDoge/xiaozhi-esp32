#include "mjpeg_player.h"
#include "audio_codec.h"
#include "config.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <esp_timer.h>

// JPEG software decoder (from esp_new_jpeg component)
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"

// MP3 simple decoder (from esp_audio_codec component)
#include "esp_audio_simple_dec.h"

// Resampler (from esp_audio_effects component)
#include "esp_ae_rate_cvt.h"

#define TAG "MjpegPlayer"

// ---------------------------------------------------------------------------
// Rate converter config macro (mirrors the one in audio_service.cc)
// ---------------------------------------------------------------------------
#define RATE_CVT_CFG_MP3(_src_rate, _dest_rate, _channel)    \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

MjpegPlayer::MjpegPlayer(esp_lcd_panel_handle_t panel, AudioCodec* codec,
                         int width, int height, int output_sample_rate)
    : panel_(panel)
    , codec_(codec)
    , display_width_(width)
    , display_height_(height)
    , output_sample_rate_(output_sample_rate)
{
}

MjpegPlayer::~MjpegPlayer() {
    Stop();
    UnmountSdCard();
}

// ---------------------------------------------------------------------------
// SD Card
// ---------------------------------------------------------------------------

bool MjpegPlayer::MountSdCard(spi_host_device_t host, gpio_num_t cs) {
    if (sd_mounted_) return true;

    static sdspi_device_config_t slot_cfg = {
        .host_id   = host,
        .gpio_cs   = cs,
        .gpio_cd   = SDSPI_SLOT_NO_CD,
        .gpio_wp   = GPIO_NUM_NC,
        .gpio_int  = GPIO_NUM_NC,
    };

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_card_t* card = nullptr;
    sdmmc_host_t sdhost = SDSPI_HOST_DEFAULT();

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &sdhost, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, card);
    sd_mounted_ = true;
    return true;
}

void MjpegPlayer::UnmountSdCard() {
    if (!sd_mounted_) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, nullptr);
    sd_mounted_ = false;
    ESP_LOGI(TAG, "SD card unmounted");
}

// ---------------------------------------------------------------------------
// File scanning
// ---------------------------------------------------------------------------

void MjpegPlayer::ScanVideoFiles(const char* dir) {
    video_files_.clear();
    DIR* d = opendir(dir);
    if (!d) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name(entry->d_name);
        ESP_LOGI(TAG, "Found entry: %s (type: %d)", name.c_str(), entry->d_type);

        if (entry->d_type == DT_DIR) continue;

        // Convert name to lowercase for checking extension
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        // Check for .mjpeg extension
        if (lower_name.size() > 6 && lower_name.substr(lower_name.size() - 6) == ".mjpeg") {
            std::string full_path = std::string(dir) + "/" + name;
            video_files_.push_back(full_path);
            ESP_LOGI(TAG, "Added video to playlist: %s", full_path.c_str());
        }
    }
    closedir(d);

    std::sort(video_files_.begin(), video_files_.end());
    ESP_LOGI(TAG, "Found %d video file(s)", video_files_.size());
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

bool MjpegPlayer::Start() {
    if (playing_.load()) {
        ESP_LOGW(TAG, "Already playing");
        return false;
    }

    if (!sd_mounted_) {
        ESP_LOGE(TAG, "SD card not mounted");
        return false;
    }

    ScanVideoFiles(VIDEO_DIR);
    if (video_files_.empty()) {
        ESP_LOGW(TAG, "No .mjpeg files found on SD card");
        return false;
    }

    current_file_index_ = 0;
    stop_requested_.store(false);
    playing_.store(true);

    xTaskCreatePinnedToCore(
        [](void* arg) {
            auto* self = static_cast<MjpegPlayer*>(arg);
            self->PlaybackTask();
            vTaskDelete(nullptr);
        },
        "mjpeg_play", 8192, this, 5, &playback_task_, 1);

    ESP_LOGI(TAG, "Playback started");
    return true;
}

void MjpegPlayer::Stop() {
    if (!playing_.load()) return;

    stop_requested_.store(true);
    audio_stop_requested_.store(true);

    // Wait for playback task to finish
    if (playback_task_) {
        // Give it time to stop
        for (int i = 0; i < 50 && playing_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        playback_task_ = nullptr;
    }

    // Wait for audio task
    if (audio_task_) {
        for (int i = 0; i < 20 && audio_task_; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        audio_task_ = nullptr;
    }

    ESP_LOGI(TAG, "Playback stopped");
}

// ---------------------------------------------------------------------------
// Playback task (video loop)
// ---------------------------------------------------------------------------

void MjpegPlayer::PlaybackTask() {
    ESP_LOGI(TAG, "Playback task running");

    while (!stop_requested_.load() && current_file_index_ < (int)video_files_.size()) {
        const std::string& video_path = video_files_[current_file_index_];

        // Derive audio path: replace .mjpeg with .mp3
        std::string audio_path = video_path.substr(0, video_path.size() - 6) + ".mp3";

        ESP_LOGI(TAG, "Playing [%d/%d]: %s", current_file_index_ + 1,
                 video_files_.size(), video_path.c_str());

        PlaySingleVideo(video_path, audio_path);

        if (!stop_requested_.load()) {
            current_file_index_++;
            if (current_file_index_ >= (int)video_files_.size()) {
                current_file_index_ = 0;  // Loop playlist
            }
        }
    }

    playing_.store(false);
    ESP_LOGI(TAG, "Playback task finished");

    if (on_finished_) {
        on_finished_();
    }
}

bool MjpegPlayer::PlaySingleVideo(const std::string& video_path,
                                   const std::string& audio_path) {
    FILE* vfp = fopen(video_path.c_str(), "rb");
    if (!vfp) {
        ESP_LOGE(TAG, "Failed to open video: %s", video_path.c_str());
        return false;
    }

    // Start audio task if MP3 file exists
    audio_stop_requested_.store(false);
    FILE* test_afp = fopen(audio_path.c_str(), "rb");
    bool has_audio = (test_afp != nullptr);
    if (test_afp) fclose(test_afp);

    if (has_audio && codec_) {
        // Enable audio output
        codec_->EnableOutput(true);

        // Pack args for audio task
        auto** ptrs = new void*[2];
        ptrs[0] = this;
        ptrs[1] = new std::string(audio_path);

        xTaskCreatePinnedToCore(
            AudioTask, "mjpeg_audio", 8192, ptrs, 4, &audio_task_, 0);
    }

    // Allocate buffers
    uint8_t* mjpeg_buf = (uint8_t*)heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mjpeg_buf) {
        // Fallback to regular malloc
        mjpeg_buf = (uint8_t*)malloc(MAX_JPEG_SIZE);
    }
    uint8_t* read_buf = (uint8_t*)malloc(READ_BUFFER_SIZE);

    if (!mjpeg_buf || !read_buf) {
        ESP_LOGE(TAG, "Failed to allocate MJPEG buffers");
        if (mjpeg_buf) free(mjpeg_buf);
        if (read_buf) free(read_buf);
        fclose(vfp);
        return false;
    }

    int buf_offset = 0;
    int buf_len = 0;
    int frame_count = 0;
    int64_t start_us = esp_timer_get_time();

    while (!stop_requested_.load()) {
        int frame_size = 0;
        if (!ReadNextJpegFrame(vfp, mjpeg_buf, &frame_size, read_buf, &buf_offset, &buf_len)) {
            break;  // EOF or error
        }

        // Frame timing
        int64_t target_us = start_us + ((int64_t)(frame_count + 1) * 1000000 / VIDEO_FPS);
        int64_t now_us = esp_timer_get_time();

        if (now_us < target_us) {
            // Decode and display
            DecodeAndDisplay(mjpeg_buf, frame_size);

            // Wait for frame timing
            now_us = esp_timer_get_time();
            if (now_us < target_us) {
                vTaskDelay(pdMS_TO_TICKS((target_us - now_us) / 1000));
            }
        } else {
            // Behind schedule — skip display but still count
            ESP_LOGD(TAG, "Skipping frame %d (behind by %lld ms)",
                     frame_count, (now_us - target_us) / 1000);
        }

        frame_count++;
    }

    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG, "Played %d frames in %lld ms (%.1f fps)",
             frame_count, elapsed_ms,
             elapsed_ms > 0 ? (frame_count * 1000.0 / elapsed_ms) : 0);

    free(mjpeg_buf);
    free(read_buf);
    fclose(vfp);

    // Stop audio
    audio_stop_requested_.store(true);

    return true;
}

// ---------------------------------------------------------------------------
// MJPEG frame extraction — scan for FFD8 (SOI) and FFD9 (EOI) markers
// ---------------------------------------------------------------------------

bool MjpegPlayer::ReadNextJpegFrame(FILE* fp, uint8_t* mjpeg_buf, int* frame_size,
                                     uint8_t* read_buf, int* buf_offset, int* buf_len) {
    *frame_size = 0;

    // Fill read buffer if empty
    if (*buf_len <= 0) {
        *buf_len = fread(read_buf, 1, READ_BUFFER_SIZE, fp);
        *buf_offset = 0;
        if (*buf_len <= 0) return false;  // EOF
    }

    // Phase 1: Find FFD8 (JPEG SOI marker)
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
            // Keep last byte (could be 0xFF)
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

    // Phase 2: Copy data until FFD9 (JPEG EOI marker) is found
    int out_offset = 0;
    bool found_eoi = false;

    while (!found_eoi) {
        int remaining = *buf_len - *buf_offset;

        // Scan for FFD9 in current buffer
        for (int i = 0; i < remaining - 1; i++) {
            int src_idx = *buf_offset + i;
            if (read_buf[src_idx] == 0xFF && read_buf[src_idx + 1] == 0xD9) {
                // Found EOI — copy up to and including FFD9
                int copy_len = i + 2;
                if (out_offset + copy_len > MAX_JPEG_SIZE) {
                    ESP_LOGE(TAG, "JPEG frame too large (>%d bytes)", MAX_JPEG_SIZE);
                    return false;
                }
                memcpy(mjpeg_buf + out_offset, read_buf + *buf_offset, copy_len);
                out_offset += copy_len;
                *buf_offset += copy_len;
                found_eoi = true;
                break;
            }
        }

        if (!found_eoi) {
            // Copy all but last byte (which might be start of FF D9)
            int copy_len = remaining - 1;
            if (copy_len > 0) {
                if (out_offset + copy_len > MAX_JPEG_SIZE) {
                    ESP_LOGE(TAG, "JPEG frame too large");
                    return false;
                }
                memcpy(mjpeg_buf + out_offset, read_buf + *buf_offset, copy_len);
                out_offset += copy_len;
            }

            // Keep last byte, refill
            read_buf[0] = read_buf[*buf_len - 1];
            *buf_len = fread(read_buf + 1, 1, READ_BUFFER_SIZE - 1, fp);
            if (*buf_len <= 0) {
                // EOF — partial frame
                return false;
            }
            *buf_len += 1;
            *buf_offset = 0;
        }
    }

    *frame_size = out_offset;
    return out_offset > 0;
}

// ---------------------------------------------------------------------------
// JPEG decode + display using esp_new_jpeg (software decoder)
// ---------------------------------------------------------------------------

bool MjpegPlayer::DecodeAndDisplay(const uint8_t* jpeg_data, int jpeg_size) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t decoder = nullptr;
    jpeg_error_t ret = jpeg_dec_open(&config, &decoder);
    if (ret != JPEG_ERR_OK || !decoder) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder: %d", ret);
        return false;
    }

    jpeg_dec_io_t io = {};
    io.inbuf = (uint8_t*)jpeg_data;
    io.inbuf_len = jpeg_size;

    jpeg_dec_header_info_t header = {};
    ret = jpeg_dec_parse_header(decoder, &io, &header);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to parse JPEG header: %d", ret);
        jpeg_dec_close(decoder);
        return false;
    }

    // Allocate output buffer
    int out_size = header.width * header.height * 2;  // RGB565
    uint8_t* out_buf = (uint8_t*)jpeg_calloc_align(out_size, 16);
    if (!out_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer (%d bytes)", out_size);
        jpeg_dec_close(decoder);
        return false;
    }

    io.outbuf = out_buf;
    ret = jpeg_dec_process(decoder, &io);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG: %d", ret);
        jpeg_free_align(out_buf);
        jpeg_dec_close(decoder);
        return false;
    }

    jpeg_dec_close(decoder);

    // Calculate centering offset if frame is smaller than display
    int offset_x = (display_width_ - header.width) / 2;
    int offset_y = (display_height_ - header.height) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    int draw_w = (header.width > display_width_) ? display_width_ : header.width;
    int draw_h = (header.height > display_height_) ? display_height_ : header.height;

    // Draw directly to LCD panel in chunks to avoid DMA memory allocation errors
    // A full 240x240 RGB565 frame is ~115KB, which exceeds internal SRAM bounce buffers.
    const int lines_per_chunk = 40; 
    for (int y = 0; y < draw_h; y += lines_per_chunk) {
        int chunk_h = (y + lines_per_chunk > draw_h) ? (draw_h - y) : lines_per_chunk;
        
        // out_buf is RGB565, 2 bytes per pixel
        uint8_t* chunk_data = out_buf + (y * header.width * 2);
        
        esp_lcd_panel_draw_bitmap(panel_,
                                  offset_x, offset_y + y,
                                  offset_x + draw_w, offset_y + y + chunk_h,
                                  chunk_data);
    }

    jpeg_free_align(out_buf);
    return true;
}

// ---------------------------------------------------------------------------
// MP3 audio playback
// ---------------------------------------------------------------------------

void MjpegPlayer::AudioTask(void* arg) {
    auto** ptrs = static_cast<void**>(arg);
    auto* self = static_cast<MjpegPlayer*>(ptrs[0]);
    auto* path = static_cast<std::string*>(ptrs[1]);

    self->AudioPlaybackLoop(*path);

    delete path;
    delete[] ptrs;
    self->audio_task_ = nullptr;
    vTaskDelete(nullptr);
}

void MjpegPlayer::AudioPlaybackLoop(const std::string& audio_path) {
    ESP_LOGI(TAG, "Audio playback starting: %s", audio_path.c_str());

    FILE* afp = fopen(audio_path.c_str(), "rb");
    if (!afp) {
        ESP_LOGW(TAG, "No audio file found: %s", audio_path.c_str());
        return;
    }

    // Open MP3 simple decoder
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type      = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg       = nullptr,
        .cfg_size      = 0,
        .use_frame_dec = false,
    };

    esp_audio_simple_dec_handle_t mp3_dec = nullptr;
    esp_audio_err_t err = esp_audio_simple_dec_open(&dec_cfg, &mp3_dec);
    if (err != ESP_AUDIO_ERR_OK || !mp3_dec) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", err);
        fclose(afp);
        return;
    }

    // Resampler (created once we know the MP3 sample rate)
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int mp3_sample_rate = 0;
    int mp3_channels = 1;

    // Buffers
    uint8_t* mp3_buf = (uint8_t*)malloc(MP3_READ_SIZE);
    // PCM output buffer — generous size for decoded frames
    int pcm_buf_size = 4608 * 2;  // MP3 max frame = 1152 samples * 2ch * 2bytes
    uint8_t* pcm_buf = (uint8_t*)malloc(pcm_buf_size);

    if (!mp3_buf || !pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        if (mp3_buf) free(mp3_buf);
        if (pcm_buf) free(pcm_buf);
        esp_audio_simple_dec_close(mp3_dec);
        fclose(afp);
        return;
    }

    int mp3_buf_offset = 0;
    int mp3_buf_len = 0;
    bool eof = false;

    while (!audio_stop_requested_.load()) {
        // Refill MP3 input buffer
        if (mp3_buf_offset >= mp3_buf_len && !eof) {
            mp3_buf_len = fread(mp3_buf, 1, MP3_READ_SIZE, afp);
            mp3_buf_offset = 0;
            if (mp3_buf_len <= 0) {
                eof = true;
            }
        }

        if (eof && mp3_buf_offset >= mp3_buf_len) {
            break;  // All data consumed
        }

        // Decode
        esp_audio_simple_dec_raw_t raw = {
            .buffer        = mp3_buf + mp3_buf_offset,
            .len           = (uint32_t)(mp3_buf_len - mp3_buf_offset),
            .eos           = eof,
            .consumed      = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        esp_audio_simple_dec_out_t out = {
            .buffer        = pcm_buf,
            .len           = (uint32_t)pcm_buf_size,
            .needed_size   = 0,
            .decoded_size  = 0,
        };

        err = esp_audio_simple_dec_process(mp3_dec, &raw, &out);
        mp3_buf_offset += raw.consumed;

        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // Reallocate PCM buffer
            pcm_buf_size = out.needed_size;
            free(pcm_buf);
            pcm_buf = (uint8_t*)malloc(pcm_buf_size);
            if (!pcm_buf) {
                ESP_LOGE(TAG, "Failed to realloc PCM buffer");
                break;
            }
            continue;  // Retry with bigger buffer
        }

        if (err != ESP_AUDIO_ERR_OK) {
            if (raw.consumed == 0 && !eof) {
                // Need more data — shift remaining to beginning
                int remaining = mp3_buf_len - mp3_buf_offset;
                if (remaining > 0) {
                    memmove(mp3_buf, mp3_buf + mp3_buf_offset, remaining);
                }
                int read = fread(mp3_buf + remaining, 1, MP3_READ_SIZE - remaining, afp);
                if (read <= 0) eof = true;
                mp3_buf_len = remaining + (read > 0 ? read : 0);
                mp3_buf_offset = 0;
            }
            continue;
        }

        if (out.decoded_size == 0) continue;

        // Get decoder info for sample rate (once)
        if (mp3_sample_rate == 0) {
            esp_audio_simple_dec_info_t info = {};
            if (esp_audio_simple_dec_get_info(mp3_dec, &info) == ESP_AUDIO_ERR_OK) {
                mp3_sample_rate = info.sample_rate;
                mp3_channels = info.channel;
                ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %d bps",
                         info.sample_rate, info.channel, info.bits_per_sample);

                // Create resampler if needed
                if (mp3_sample_rate != output_sample_rate_) {
                    esp_ae_rate_cvt_cfg_t cvt_cfg = RATE_CVT_CFG_MP3(
                        mp3_sample_rate, output_sample_rate_, 1);
                    esp_ae_rate_cvt_open(&cvt_cfg, &resampler);
                    if (!resampler) {
                        ESP_LOGE(TAG, "Failed to create audio resampler");
                    }
                }
            }
        }

        // Convert to int16_t samples
        int16_t* pcm_samples = (int16_t*)pcm_buf;
        int sample_count = out.decoded_size / sizeof(int16_t);

        // If stereo, downmix to mono
        if (mp3_channels == 2) {
            for (int i = 0; i < sample_count / 2; i++) {
                pcm_samples[i] = (int16_t)(((int32_t)pcm_samples[i * 2] +
                                             (int32_t)pcm_samples[i * 2 + 1]) / 2);
            }
            sample_count /= 2;
        }

        // Resample if needed
        std::vector<int16_t> output_data;
        if (resampler && mp3_sample_rate != output_sample_rate_) {
            uint32_t out_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(resampler, sample_count, &out_samples);
            output_data.resize(out_samples);
            uint32_t actual_out = out_samples;
            esp_ae_rate_cvt_process(resampler,
                                    (esp_ae_sample_t)pcm_samples, sample_count,
                                    (esp_ae_sample_t)output_data.data(), &actual_out);
            output_data.resize(actual_out);
        } else {
            output_data.assign(pcm_samples, pcm_samples + sample_count);
        }

        // Feed through AudioCodec
        if (codec_ && !output_data.empty()) {
            codec_->OutputData(output_data);
        }
    }

    // Cleanup
    if (resampler) esp_ae_rate_cvt_close(resampler);
    free(mp3_buf);
    free(pcm_buf);
    esp_audio_simple_dec_close(mp3_dec);
    fclose(afp);

    ESP_LOGI(TAG, "Audio playback finished: %s", audio_path.c_str());
}
