#pragma once
#include "base_app.h"
#include "mjpeg_player.h"

class Display;

class PowerSaveTimer;

class VideoApp : public BaseApp {
private:
    MjpegPlayer* mjpeg_player_ = nullptr;
    Display* display_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    bool video_mode_ = false;
public:
    VideoApp(MjpegPlayer* player, Display* display, PowerSaveTimer* timer);
    ~VideoApp() override = default;
    
    void OnStart() override;
    void OnStop() override;
    void OnPlaybackFinished();
};
