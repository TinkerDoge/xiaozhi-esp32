#pragma once
#include "base_app.h"
#include "mjpeg_player.h"

class VideoApp : public BaseApp {
private:
    MjpegPlayer* mjpeg_player_ = nullptr;
    bool video_mode_ = false;
public:
    VideoApp(MjpegPlayer* player);
    ~VideoApp() override = default;
    
    void OnStart() override;
    void OnStop() override;
};
