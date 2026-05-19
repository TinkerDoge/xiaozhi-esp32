#pragma once
#include "base_app.h"

class AiApp : public BaseApp {
private:
    bool conversation_active_ = false;
public:
    AiApp();
    ~AiApp() override = default;
    
    void OnStart() override;
    void OnStop() override;
    void OnButtonAClick() override;
};
