#include "video_app.h"
#include "application.h"
#include "display/display.h"
#include <esp_log.h>
#include <esp_lvgl_port.h>

static const char* TAG = "VideoApp";

VideoApp::VideoApp(MjpegPlayer* player, Display* display) : mjpeg_player_(player), display_(display) {}

void VideoApp::OnStart() {
    ESP_LOGI(TAG, "Starting Video App");
    if (!mjpeg_player_ || !mjpeg_player_->IsSdMounted()) {
        if (display_) display_->ShowNotification("No SD");
        return;
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
    }
}

void VideoApp::OnStop() {
    ESP_LOGI(TAG, "Stopping Video App");
    if (video_mode_ && mjpeg_player_) {
        mjpeg_player_->Stop();
        video_mode_ = false;
    }
}
