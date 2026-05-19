#include "wifi_board.h"
#include "audio/codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "power_save_timer.h"
#include "adc_battery_monitor.h"
#include "assets/lang_config.h"
#include "mjpeg_player.h"
#include "base_app.h"
#include "ai_app.h"
#include "video_app.h"
#include <esp_lvgl_port.h>

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#define TAG "DogePet"

class DogePet : public WifiBoard {
private:
    Button boot_button_;
    Button btn_a_;
    Button btn_b_;
    BaseApp* current_app_ = nullptr;
    AiApp* ai_app_ = nullptr;
    VideoApp* video_app_ = nullptr;
    LcdDisplay* display_ = nullptr;
    esp_lcd_panel_handle_t raw_panel_ = nullptr;  // raw panel handle for direct video rendering
    PowerSaveTimer* power_save_timer_ = nullptr;
    AdcBatteryMonitor* adc_battery_monitor_ = nullptr;
    MjpegPlayer* mjpeg_player_ = nullptr;

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
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
#ifdef DISPLAY_INVERT_COLOR
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
#else
        esp_lcd_panel_invert_color(panel, true);
#endif
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // Store raw panel handle for video player direct rendering
        raw_panel_ = panel;

        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeSDcardSpi() {
        // SD card shares SPI3_HOST with display, only needs its own CS pin
        mjpeg_player_ = new MjpegPlayer(
            raw_panel_, GetAudioCodec(),
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            AUDIO_OUTPUT_SAMPLE_RATE);

        if (!mjpeg_player_->MountSdCard(SD_SPI_HOST, SD_CARD_CS_PIN)) {
            ESP_LOGW(TAG, "SD card not available — video player disabled");
        }

        // When playback finishes, restore LVGL display
        mjpeg_player_->OnFinished([this]() {
            lvgl_port_resume(); // Resume LVGL rendering loop
            if (display_) {
                display_->SetPowerSaveMode(false);  // Re-enable LVGL rendering
            }
            if (power_save_timer_) power_save_timer_->SetEnabled(true);
            ESP_LOGI(TAG, "Video playback finished, LVGL restored");
        });
    }

    void SwitchApp() {
        if (!current_app_) return;
        current_app_->OnStop();
        
        if (current_app_ == ai_app_) {
            current_app_ = video_app_;
        } else {
            current_app_ = ai_app_;
        }
        
        current_app_->OnStart();
    }

    void InitializeButtons() {
        // Boot button: long press enters Wi-Fi config; short press just wakes
        boot_button_.OnClick([this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
        });
        boot_button_.OnLongPress([this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            EnterWifiConfigMode();
        });

        // Button A: click = toggle AI conversation, long press = sleep
        btn_a_.OnClick([this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
            if (current_app_) current_app_->OnButtonAClick();
        });
        btn_a_.OnLongPress([this]() {
            // Say goodbye and enter sleep mode
            if (display_) display_->ShowNotification("BYE");
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
            if (current_app_) current_app_->OnStop();
            if (auto d = GetDisplay()) d->SetPowerSaveMode(true);
            if (auto bl = GetBacklight()) bl->SetBrightness(0);
        });

        // Button B: click = wake, long press = toggle video player mode
        btn_b_.OnClick([this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
            if (current_app_) current_app_->OnButtonBClick();
        });
        btn_b_.OnLongPress([this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
            SwitchApp();
        });
    }



public:
    DogePet() :
        boot_button_(BOOT_BUTTON_GPIO),
        btn_a_(BUTTON_A_GPIO),
        btn_b_(BUTTON_B_GPIO) {
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        InitializeSDcardSpi();
        // Battery monitoring (1M/1M voltage divider on GPIO15)
        adc_battery_monitor_ = new AdcBatteryMonitor(VBAT_ADC_UNIT, VBAT_ADC_CH, VBAT_UPPER_R, VBAT_LOWER_R, GPIO_NUM_NC);
        // Idle power save: screen dims/sleeps when idle; restore on activity
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            if (current_app_) current_app_->OnStop();
            if (display_) display_->ShowNotification("BYE");
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
            if (auto bl = GetBacklight()) bl->SetBrightness(5);
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

        ai_app_ = new AiApp();
        video_app_ = new VideoApp(mjpeg_player_);
        current_app_ = ai_app_;
        current_app_->OnStart();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // Standard duplex codec for shared-clock INMP441 + MAX98357A
#ifdef AUDIO_I2S_METHOD_DUPLEX
        static NoAudioCodecDuplex audio_codec(
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

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::PERFORMANCE) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        if (!adc_battery_monitor_) return false;
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(DogePet);
