#include "wifi_board.h"
#include "audio/codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "power_save_timer.h"
#include "assets/lang_config.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib>

#define TAG "DogePet"

class DogePetDuplexCodec : public NoAudioCodec {
public:
    DogePetDuplexCodec(int input_sample_rate, int output_sample_rate,
                       gpio_num_t bclk, gpio_num_t ws,
                       gpio_num_t dout, gpio_num_t din) {
        duplex_ = true;
        input_reference_ = AUDIO_INPUT_REFERENCE;
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
        input_channels_ = input_reference_ ? 2 : 1;
        output_channels_ = 2;

        i2s_chan_config_t chan_cfg = {
            .id = I2S_NUM_0,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
        };
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

        i2s_std_config_t tx_cfg = {
            .clk_cfg = {
                .sample_rate_hz = (uint32_t)output_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
                .ext_clk_freq_hz = 0,
#endif
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_STD_SLOT_BOTH,
                .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
                .ws_pol = false,
                .bit_shift = true,
#ifdef I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false,
#endif
            },
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = bclk,
                .ws = ws,
                .dout = dout,
                .din = din,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_cfg));

        auto rx_cfg = tx_cfg;
        rx_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
        // INMP441 with L/R=VCC outputs on right channel only
        // Configure RX to read right slot in mono mode
#ifdef AUDIO_I2S_MIC_SLOT_INDEX
        #if AUDIO_I2S_MIC_SLOT_INDEX == 1
        rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
        #else
        rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
        #endif
        rx_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
        rx_slot_count_ = 1;
        active_input_slot_ = AUDIO_I2S_MIC_SLOT_INDEX;
        mic_slot_locked_ = true;
#else
        // Fallback: read both slots and auto-detect
        if (input_reference_) {
            rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
            rx_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
            rx_slot_count_ = 2;
        } else {
            rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
            rx_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
            rx_slot_count_ = 1;
            active_input_slot_ = 0;
            mic_slot_locked_ = true;
        }
#endif
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_cfg));
        
        // NOTE: Don't enable channels here - AudioCodec::Start() will do it
        ESP_LOGI(TAG, "DogePet duplex codec initialized");
        ESP_LOGI(TAG, "[AUDIO] I2S Config: BCLK=%d, WS=%d, DOUT=%d, DIN=%d",
                 AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        ESP_LOGI(TAG, "[AUDIO] Sample rates: IN=%d, OUT=%d", input_sample_rate_, output_sample_rate_);
        ESP_LOGI(TAG, "[AUDIO] RX slot count=%d, input_reference=%d", rx_slot_count_, input_reference_ ? 1 : 0);
    }

protected:
    virtual int Write(const int16_t* data, int samples) override {
        static int write_log_counter = 0;
        if (++write_log_counter % 100 == 1) {
            // Find peak sample for logging
            int16_t peak = 0;
            for (int i = 0; i < samples; ++i) {
                if (std::abs(data[i]) > std::abs(peak)) peak = data[i];
            }
            ESP_LOGI(TAG, "[AUDIO TX] samples=%d, volume=%d, peak_in=%d", samples, output_volume_, peak);
        }
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        std::vector<int32_t> buffer(samples * 2);

        int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
        const int32_t soft_limit_thresh = INT32_MAX * 0.85f;  // Start soft limiting at 85%
        
        for (int i = 0; i < samples; ++i) {
            int64_t temp = int64_t(data[i]) * volume_factor;
            
#ifdef AUDIO_OUTPUT_LIMITER
            // Soft limiting to prevent harsh clipping
            if (std::abs(temp) > soft_limit_thresh) {
                float over = (std::abs(temp) - soft_limit_thresh) / (float)(INT32_MAX - soft_limit_thresh);
                float compress = 1.0f / (1.0f + over * 2.0f);  // Soft knee compression
                temp = soft_limit_thresh + (temp - soft_limit_thresh) * compress;
            }
#endif
            
            if (temp > INT32_MAX) temp = INT32_MAX;
            else if (temp < INT32_MIN) temp = INT32_MIN;
            int32_t sample32 = static_cast<int32_t>(temp);
            buffer[i * 2] = sample32;
            buffer[i * 2 + 1] = sample32;
        }

        size_t bytes_written;
        ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), buffer.size() * sizeof(int32_t), &bytes_written, portMAX_DELAY));
        return bytes_written / (sizeof(int32_t) * 2);
    }

    virtual int Read(int16_t* dest, int samples) override {
        static int read_log_counter = 0;
        const int slot_count = rx_slot_count_;
        std::vector<int32_t> bit32_buffer(samples * slot_count);
        size_t bytes_read;
        esp_err_t err = i2s_channel_read(rx_handle_, bit32_buffer.data(), bit32_buffer.size() * sizeof(int32_t), &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[AUDIO RX] Read Failed! err=%d", err);
            return 0;
        }
        
        // Periodic logging of mic input levels
        if (++read_log_counter % 100 == 1) {
            int32_t peak_raw = 0;
            int64_t sum_left = 0, sum_right = 0;
            int frames_check = bytes_read / (sizeof(int32_t) * slot_count);
            for (int i = 0; i < frames_check && i < 100; ++i) {
                int32_t val = bit32_buffer[i * slot_count] >> 14;
                if (std::abs(val) > std::abs(peak_raw)) peak_raw = val;
                if (slot_count == 2) {
                    sum_left += llabs(bit32_buffer[i * 2] >> 14);
                    sum_right += llabs(bit32_buffer[i * 2 + 1] >> 14);
                }
            }
            ESP_LOGI(TAG, "[AUDIO RX] bytes=%d, slot_count=%d, active_slot=%d, peak_raw=%ld, L_avg=%lld, R_avg=%lld",
                     (int)bytes_read, slot_count, active_input_slot_, (long)peak_raw,
                     frames_check > 0 ? sum_left / frames_check : 0,
                     frames_check > 0 ? sum_right / frames_check : 0);
        }

        int frames = bytes_read / (sizeof(int32_t) * slot_count);
        if (slot_count == 2 && !mic_slot_locked_ && frames > 0) {
            int64_t channel_energy[2] = {0, 0};
            for (int i = 0; i < frames; ++i) {
                channel_energy[0] += llabs((int64_t)bit32_buffer[i * 2]);
                channel_energy[1] += llabs((int64_t)bit32_buffer[i * 2 + 1]);
            }
            int best_slot = (channel_energy[1] > channel_energy[0]) ? 1 : 0;
            int64_t best_energy = channel_energy[best_slot];
            int64_t other_energy = channel_energy[1 - best_slot];
            if (best_energy > 4096 || best_energy > other_energy * 2) {
                active_input_slot_ = best_slot;
                mic_slot_locked_ = true;
                ESP_LOGI(TAG, "Detected mic data on %s slot", active_input_slot_ == 0 ? "left" : "right");
            }
        }
        // High-pass filter coefficient (80Hz @ 24kHz sample rate)
        const float alpha = 0.979f;  // RC filter: alpha = 1 - (2*pi*fc/fs)
        const float gain = AUDIO_INPUT_GAIN;
        const int16_t noise_gate = AUDIO_NOISE_GATE_THRESH;
        
        for (int i = 0; i < frames; ++i) {
            int slot_offset = (slot_count == 2) ? active_input_slot_ : 0;
            // INMP441 outputs 24-bit data in 32-bit frame, MSB-aligned
            // Shift right by 14 to get ~18-bit range, then processing reduces to 16-bit
            int32_t raw = bit32_buffer[i * slot_count + slot_offset] >> 14;
            
            // DC offset removal (slow-moving average)
            dc_offset_ = dc_offset_ * 0.999f + raw * 0.001f;
            float sample = raw - dc_offset_;
            
            // High-pass filter to remove low-frequency rumble
            hpf_prev_output_ = alpha * (hpf_prev_output_ + sample - hpf_prev_input_);
            hpf_prev_input_ = sample;
            sample = hpf_prev_output_;
            
            // Apply input gain
            sample *= gain;
            
            // Simple noise gate
            if (noise_gate > 0) {
                float abs_val = std::abs(sample);
                if (abs_val < noise_gate) {
                    sample *= (abs_val / noise_gate);  // Gradual fade below threshold
                }
            }
            
            // Clamp to int16 range
            if (sample > INT16_MAX) sample = INT16_MAX;
            else if (sample < -INT16_MAX) sample = -INT16_MAX;
            
            dest[i] = static_cast<int16_t>(sample);
        }
        return frames;
    }

private:
    int rx_slot_count_ = 1;
    int active_input_slot_ = 0;
    bool mic_slot_locked_ = false;
    
    // Audio quality filters
    float dc_offset_ = 0.0f;
    float hpf_prev_input_ = 0.0f;
    float hpf_prev_output_ = 0.0f;
    int32_t peak_hold_ = 0;
};

class DogePet : public WifiBoard {
private:
    Button boot_button_;
    Button btn_a_;
    Button btn_b_;
    Button btn_c_;
    bool conversation_active_ = false;
    LcdDisplay* display_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    // IMU removed to save space

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MISO_PIN;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
    // Honor per-panel invert setting when provided, fallback to true for typical ST7789 1.54" panels
#ifdef DISPLAY_INVERT_COLOR
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
#else
    esp_lcd_panel_invert_color(panel, true);
#endif
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        ESP_LOGI(TAG, "Initializing buttons: BOOT=%d, A=%d, B=%d, C=%d",
                 BOOT_BUTTON_GPIO, BUTTON_A_GPIO, BUTTON_B_GPIO, BUTTON_C_GPIO);
        // Boot button: long press enters Wi-Fi config; short press just wakes
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "[BUTTON] BOOT clicked");
            if (power_save_timer_) power_save_timer_->WakeUp();
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "[BUTTON] BOOT long press - entering WiFi config");
            if (power_save_timer_) power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            ResetWifiConfiguration();
        });

        btn_a_.OnClick([this]() {
            ESP_LOGI(TAG, "[BUTTON] A clicked - toggle conversation");
            if (power_save_timer_) power_save_timer_->WakeUp();
            ToggleConversationMode();
        });
        btn_a_.OnLongPress([this]() {
            ESP_LOGI(TAG, "[BUTTON] A long press - toggle conversation");
            if (power_save_timer_) power_save_timer_->WakeUp();
            ToggleConversationMode();
        });

        btn_b_.OnClick([this]() {
            ESP_LOGI(TAG, "[BUTTON] B clicked - volume down");
            if (power_save_timer_) power_save_timer_->WakeUp();
            ChangeVolumeBy(-10);
        });
        btn_b_.OnLongPress([this]() {
            ESP_LOGI(TAG, "[BUTTON] B long press - mute");
            if (power_save_timer_) power_save_timer_->WakeUp();
            SetVolumeAndNotify(0, "MUTED");
        });

        btn_c_.OnClick([this]() {
            ESP_LOGI(TAG, "[BUTTON] C clicked - volume up");
            if (power_save_timer_) power_save_timer_->WakeUp();
            ChangeVolumeBy(10);
        });
        btn_c_.OnLongPress([this]() {
            ESP_LOGI(TAG, "[BUTTON] C long press - max volume");
            if (power_save_timer_) power_save_timer_->WakeUp();
            SetVolumeAndNotify(100, "MAX VOL");
        });
    }

    void ToggleConversationMode() {
        if (!conversation_active_) {
            Application::GetInstance().StartListening();
            conversation_active_ = true;
            if (display_) display_->ShowNotification("AI ON");
        } else {
            Application::GetInstance().StopListening();
            conversation_active_ = false;
            if (display_) display_->ShowNotification("AI OFF");
        }
    }

    void ChangeVolumeBy(int delta) {
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() + delta;
        if (volume > 100) volume = 100;
        if (volume < 0) volume = 0;
        codec->SetOutputVolume(volume);
        ShowVolumeNotification(volume);
    }

    void SetVolumeAndNotify(int volume, const char* custom_message) {
        if (volume > 100) volume = 100;
        if (volume < 0) volume = 0;
        auto codec = GetAudioCodec();
        codec->SetOutputVolume(volume);
        if (display_) {
            if (custom_message) {
                display_->ShowNotification(custom_message);
            } else {
                ShowVolumeNotification(volume);
            }
        }
    }

    void ShowVolumeNotification(int volume) {
        if (!display_) return;
        display_->ShowNotification(std::string("VOL ") + std::to_string(volume / 10));
    }

    // IMU functions removed

public:
    DogePet() :
        boot_button_(BOOT_BUTTON_GPIO),
        btn_a_(BUTTON_A_GPIO),
        btn_b_(BUTTON_B_GPIO),
        btn_c_(BUTTON_C_GPIO) {
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        // Idle power save: screen dims/sleeps when idle; restore on activity
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            // Friendly goodbye on sleep
            if (display_) display_->ShowNotification("BYE");
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
            if (auto bl = GetBacklight()) bl->SetBrightness(5);  // 5% backlight when sleeping
            if (auto d = GetDisplay()) d->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            if (auto d = GetDisplay()) d->SetPowerSaveMode(false);
            if (auto bl = GetBacklight()) bl->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }

        // IMU-related MCP tools removed to save space
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // DogePet ships with shared-clock INMP441 + MAX98357A wiring.
        // The custom duplex codec keeps TX in stereo slot mode so BCLK/WS waveforms
        // stay compatible with both devices and prevents the clipping/distortion we saw
        // when the default mono-only codec halved the LR rate.
#ifdef AUDIO_I2S_METHOD_DUPLEX
        static DogePetDuplexCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
#elif defined(AUDIO_I2S_METHOD_SIMPLEX)
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS,
            AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
#else
#error "Select either AUDIO_I2S_METHOD_DUPLEX or AUDIO_I2S_METHOD_SIMPLEX in board config"
#endif
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(DogePet);
