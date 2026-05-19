#pragma once
#include "base_app.h"

class Display;

class AiApp : public BaseApp {
private:
    bool conversation_active_ = false;
    Display* display_ = nullptr;
public:
    AiApp(Display* display);
    ~AiApp() override = default;
    
    void OnStart() override;
    void OnStop() override;
    void OnButtonAClick() override;
};
