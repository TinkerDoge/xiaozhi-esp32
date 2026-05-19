#pragma once
#include "base_app.h"
#include "mjpeg_player.h"

class Display;

class VideoApp : public BaseApp {
private:
    MjpegPlayer* mjpeg_player_ = nullptr;
    Display* display_ = nullptr;
    bool video_mode_ = false;
public:
    VideoApp(MjpegPlayer* player, Display* display);
    ~VideoApp() override = default;
    
    void OnStart() override;
    void OnStop() override;
};
