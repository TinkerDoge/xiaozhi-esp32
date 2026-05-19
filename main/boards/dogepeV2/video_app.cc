#include "video_app.h"
#include "application.h"
#include "display/display.h"
#include <esp_log.h>
#include <esp_lvgl_port.h>

#include "power_save_timer.h"
#include <esp_wifi.h>

static const char* TAG = "VideoApp";

VideoApp::VideoApp(MjpegPlayer* player, Display* display, PowerSaveTimer* timer) 
    : mjpeg_player_(player), display_(display), power_save_timer_(timer) {}

void VideoApp::OnStart() {
    ESP_LOGI(TAG, "Starting Video App");
    if (!mjpeg_player_ || !mjpeg_player_->IsSdMounted()) {
        if (display_) display_->ShowNotification("No SD");
        return;
    }
    
    // Completely isolate video app by suspending AI/WiFi interference
    esp_wifi_stop();
    if (power_save_timer_) {
        power_save_timer_->WakeUp();
        power_save_timer_->SetEnabled(false);
    }
    
    if (display_) {
        display_->SetPowerSaveMode(true);
    }
    lvgl_port_stop(); // Pause LVGL rendering loop to free SPI bus

    video_mode_ = true;
    if (!mjpeg_player_->Start()) {
        video_mode_ = false;
        lvgl_port_resume();
        if (display_) {
            display_->SetPowerSaveMode(false);
            display_->ShowNotification("No Video");
        }
        if (power_save_timer_) power_save_timer_->SetEnabled(true);
        esp_wifi_start();
    }
}

void VideoApp::OnStop() {
    ESP_LOGI(TAG, "Stopping Video App");
    if (video_mode_ && mjpeg_player_) {
        mjpeg_player_->Stop();
        video_mode_ = false;
    }
    // Restore AI/WiFi functions
    esp_wifi_start();
    if (power_save_timer_) power_save_timer_->SetEnabled(true);
}
