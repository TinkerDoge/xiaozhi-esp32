# DogePetV2 App Switcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple the DogePetV2 AI and Video functionalities into independent apps managed by a central router to prevent resource conflicts.

**Architecture:** We will implement an App Manager pattern with a `BaseApp` interface. The AI and Video logic will be extracted from `dogepetv2.cc` into `ai_app.cc` and `video_app.cc`. The `DogePet` board class will manage the active app and route inputs to it.

**Tech Stack:** C++, ESP-IDF, LVGL

---

### Task 1: Create BaseApp Interface

**Files:**
- Create: `main/boards/dogepeV2/base_app.h`

- [ ] **Step 1: Write `base_app.h`**

```cpp
#pragma once

class BaseApp {
public:
    virtual ~BaseApp() = default;
    
    virtual void OnStart() = 0;
    virtual void OnStop() = 0;
    
    virtual void OnButtonAClick() {}
    virtual void OnButtonALongPress() {}
    virtual void OnButtonBClick() {}
};
```

- [ ] **Step 2: Commit**

```bash
git add main/boards/dogepeV2/base_app.h
git commit -m "feat: add BaseApp interface for DogePetV2 apps"
```

### Task 2: Create AiApp

**Files:**
- Create: `main/boards/dogepeV2/ai_app.h`
- Create: `main/boards/dogepeV2/ai_app.cc`

- [ ] **Step 1: Write `ai_app.h`**

```cpp
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
```

- [ ] **Step 2: Write `ai_app.cc`**

```cpp
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
```

- [ ] **Step 3: Commit**

```bash
git add main/boards/dogepeV2/ai_app.h main/boards/dogepeV2/ai_app.cc
git commit -m "feat: implement AiApp logic"
```

### Task 3: Create VideoApp

**Files:**
- Create: `main/boards/dogepeV2/video_app.h`
- Create: `main/boards/dogepeV2/video_app.cc`

- [ ] **Step 1: Write `video_app.h`**

```cpp
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
```

- [ ] **Step 2: Write `video_app.cc`**

```cpp
#include "video_app.h"
#include "application.h"
#include <esp_log.h>
#include <esp_lvgl_port.h>

static const char* TAG = "VideoApp";

VideoApp::VideoApp(MjpegPlayer* player) : mjpeg_player_(player) {}

void VideoApp::OnStart() {
    ESP_LOGI(TAG, "Starting Video App");
    auto* display = Application::GetInstance().GetBoard().GetDisplay();
    if (!mjpeg_player_ || !mjpeg_player_->IsSdMounted()) {
        if (display) display->ShowNotification("No SD");
        return;
    }
    
    if (display) {
        display->SetPowerSaveMode(true);
    }
    lvgl_port_stop(); // Pause LVGL rendering loop to free SPI bus

    video_mode_ = true;
    if (!mjpeg_player_->Start()) {
        video_mode_ = false;
        lvgl_port_resume();
        if (display) {
            display->SetPowerSaveMode(false);
            display->ShowNotification("No Video");
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
```

- [ ] **Step 3: Commit**

```bash
git add main/boards/dogepeV2/video_app.h main/boards/dogepeV2/video_app.cc
git commit -m "feat: implement VideoApp logic"
```

### Task 4: Refactor DogePet Router

**Files:**
- Modify: `main/boards/dogepeV2/dogepetv2.cc`

- [ ] **Step 1: Add new includes to `dogepetv2.cc`**
```cpp
#include "base_app.h"
#include "ai_app.h"
#include "video_app.h"
```

- [ ] **Step 2: Update member variables in DogePet class**
Remove `conversation_active_` and `video_mode_`.
Add:
```cpp
    BaseApp* current_app_ = nullptr;
    AiApp* ai_app_ = nullptr;
    VideoApp* video_app_ = nullptr;
```

- [ ] **Step 3: Add `SwitchApp()` function**
Add inside the class before `InitializeButtons()`:
```cpp
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
```

- [ ] **Step 4: Update logic functions**
Delete `ToggleConversationMode()` and `ToggleVideoMode()`.

In `InitializeSDcardSpi()`, replace `video_mode_ = false;` logic in `OnFinished` callback with:
```cpp
        mjpeg_player_->OnFinished([this]() {
            lvgl_port_resume();
            if (display_) display_->SetPowerSaveMode(false);
            if (power_save_timer_) power_save_timer_->SetEnabled(true);
            ESP_LOGI(TAG, "Video playback finished, LVGL restored");
        });
```

- [ ] **Step 5: Update Button routing in `InitializeButtons()`**
Update the lambdas:
- `btn_a_.OnClick`: Call `if (current_app_) current_app_->OnButtonAClick();`
- `btn_a_.OnLongPress`: Call `if (current_app_) current_app_->OnStop();` instead of old conversation code.
- `btn_b_.OnClick`: Call `if (current_app_) current_app_->OnButtonBClick();`
- `btn_b_.OnLongPress`: Call `SwitchApp();`

- [ ] **Step 6: Update Boot/Sleep Initialization**
In constructor, under power save mode enter callback, call `if (current_app_) current_app_->OnStop();`.
At the very end of the constructor, initialize the apps:
```cpp
        ai_app_ = new AiApp();
        video_app_ = new VideoApp(mjpeg_player_);
        current_app_ = ai_app_;
        current_app_->OnStart();
```

- [ ] **Step 7: Commit**

```bash
git add main/boards/dogepeV2/dogepetv2.cc
git commit -m "refactor: integrate App Manager into DogePet board"
```
