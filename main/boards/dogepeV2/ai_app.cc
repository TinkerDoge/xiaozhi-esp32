#include "ai_app.h"
#include "application.h"
#include <esp_log.h>
#include <esp_lvgl_port.h>

static const char* TAG = "AiApp";

AiApp::AiApp() {}

void AiApp::OnStart() {
    ESP_LOGI(TAG, "Starting AI App");
    lvgl_port_resume();
    auto* display = Application::GetInstance().GetBoard().GetDisplay();
    if (display) {
        display->SetPowerSaveMode(false);
    }
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
}

void AiApp::OnStop() {
    ESP_LOGI(TAG, "Stopping AI App");
    if (conversation_active_) {
        Application::GetInstance().StopListening();
        conversation_active_ = false;
    }
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}

void AiApp::OnButtonAClick() {
    auto* display = Application::GetInstance().GetBoard().GetDisplay();
    if (!conversation_active_) {
        Application::GetInstance().StartListening();
        conversation_active_ = true;
        if (display) display->ShowNotification("AI ON");
    } else {
        Application::GetInstance().StopListening();
        conversation_active_ = false;
        if (display) display->ShowNotification("AI OFF");
    }
}
