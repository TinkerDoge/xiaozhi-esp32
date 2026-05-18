#ifndef _MJPEG_PLAYER_H_
#define _MJPEG_PLAYER_H_

#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>
#include <atomic>
#include <functional>

// Forward declarations
class AudioCodec;

/**
 * @brief MJPEG video + MP3 audio player for SD card media.
 *
 * Plays .mjpeg video files (with optional paired .mp3 audio) from an SD card
 * mounted via SPI VFS. Video frames are decoded using esp_jpeg_dec and rendered
 * directly to an esp_lcd_panel, bypassing LVGL during playback.
 *
 * Audio is decoded via esp_audio_simple_dec (MP3), resampled to the board's
 * output sample rate, and fed through the existing AudioCodec::OutputData() pipeline.
 */
class MjpegPlayer {
public:
    /**
     * @param panel      LCD panel handle for direct bitmap writes
     * @param codec      Audio codec for PCM output (may be nullptr for video-only)
     * @param width      Display width in pixels
     * @param height     Display height in pixels
     * @param output_sample_rate  Audio codec output sample rate (e.g. 24000)
     */
    MjpegPlayer(esp_lcd_panel_handle_t panel, AudioCodec* codec,
                int width, int height, int output_sample_rate);
    ~MjpegPlayer();

    /**
     * @brief Mount the SD card via SPI to VFS.
     * @param host  SPI host (e.g. SPI3_HOST) — must already be initialized
     * @param cs    CS GPIO for the SD card
     * @return true if mounted successfully
     */
    bool MountSdCard(spi_host_device_t host, gpio_num_t cs);

    /** Unmount SD card from VFS */
    void UnmountSdCard();

    /** @return true if SD card is mounted */
    bool IsSdMounted() const { return sd_mounted_; }

    /**
     * @brief Scan the video directory and start playback from the first file.
     *        Creates a dedicated FreeRTOS task for the playback loop.
     * @return true if at least one video was found and playback started
     */
    bool Start();

    /** Stop playback and wait for the task to finish. */
    void Stop();

    /** @return true if currently playing video */
    bool IsPlaying() const { return playing_.load(); }

    /** Callback invoked when playback finishes (all files played or stopped) */
    using OnFinishedCallback = std::function<void()>;
    void OnFinished(OnFinishedCallback cb) { on_finished_ = cb; }

private:
    // Hardware
    esp_lcd_panel_handle_t panel_;
    AudioCodec* codec_;
    int display_width_;
    int display_height_;
    int output_sample_rate_;

    // SD card
    bool sd_mounted_ = false;

    // Playback state
    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t playback_task_ = nullptr;
    OnFinishedCallback on_finished_;

    // Media file list
    std::vector<std::string> video_files_;
    int current_file_index_ = 0;

    // Buffers
    static constexpr int READ_BUFFER_SIZE = 1024;
    static constexpr int MAX_JPEG_SIZE = 120 * 1024;  // max single JPEG frame
    static constexpr int MP3_READ_SIZE = 2048;

    // Internal methods
    void ScanVideoFiles(const char* dir);
    void PlaybackTask();
    bool PlaySingleVideo(const std::string& video_path, const std::string& audio_path);

    // MJPEG parsing
    bool ReadNextJpegFrame(FILE* fp, uint8_t* mjpeg_buf, int* frame_size,
                           uint8_t* read_buf, int* buf_offset, int* buf_len);

    // JPEG decoding + display
    bool DecodeAndDisplay(const uint8_t* jpeg_data, int jpeg_size);

    // MP3 audio playback (runs on separate task)
    static void AudioTask(void* arg);
    void AudioPlaybackLoop(const std::string& audio_path);
    TaskHandle_t audio_task_ = nullptr;
    std::atomic<bool> audio_stop_requested_{false};
};

#endif // _MJPEG_PLAYER_H_
