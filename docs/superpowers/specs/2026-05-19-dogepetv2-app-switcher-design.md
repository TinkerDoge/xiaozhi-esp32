# DogePetV2 App Switcher Architecture

## Context
The DogePetV2 board currently houses all application logic (AI voice assistant and Video player) inside the main `DogePet` board class. This has resulted in a monolithic file (`dogepetv2.cc`) where hardware initialization and app logic are intertwined, causing resource conflicts and violating the project's modularity guidelines.

## Goal
To decouple the AI and Video functionalities into independent, isolated "Apps" that manage their own state. A master App Switcher in the main board class will toggle between them using a hardware button, ensuring they do not affect each other's execution or background processes.

## Architecture: App Manager Pattern

### 1. The `BaseApp` Interface
All apps must implement a standard interface that abstracts their lifecycle and input handling away from the hardware.

```cpp
class BaseApp {
public:
    virtual ~BaseApp() = default;
    
    // Lifecycle
    virtual void OnStart() = 0;
    virtual void OnStop() = 0;
    
    // Input Routing
    virtual void OnButtonAClick() {}
    virtual void OnButtonALongPress() {}
    virtual void OnButtonBClick() {}
};
```

### 2. App Implementations

#### `AiApp`
Encapsulates the voice assistant logic.
- **OnStart:** Enables wake word detection and ensures LVGL rendering is active for the UI.
- **OnStop:** Forces any active conversation to stop (`StopListening()`) and disables the wake word engine to free CPU/RAM.
- **Input Routing:** Button A triggers the conversational toggle (Start/Stop listening).

#### `VideoApp`
Encapsulates the `MjpegPlayer`.
- **OnStart:** Pauses LVGL rendering (`lvgl_port_stop()`) to completely free the SPI bus, verifies the SD card, and begins video playback.
- **OnStop:** Halts video playback gracefully.
- **Input Routing:** Button A can be mapped to Play/Pause logic in the future.

### 3. Board Router (`DogePet`)
The main `DogePet` class becomes an App Host.
- **Responsibilities:** Retains initialization of hardware (Display, SD SPI, Audio Codec, Buttons).
- **State Management:** Maintains a pointer `BaseApp* current_app_`. On boot, it instantiates `AiApp` and `VideoApp` and sets `AiApp` as active by calling its `OnStart()`.
- **Switching Logic:** A long press on Button B triggers `SwitchApp()`. This method calls `OnStop()` on the current app, swaps the pointer, and calls `OnStart()` on the newly active app.
- **Input Forwarding:** Standard button clicks are blindly forwarded to `current_app_`.

## Trade-offs
This approach adds slightly more boilerplate file structure but solves the complexity scaling issues and perfectly adheres to the project's modularity requirements.

## Milestones
1. Scaffold `base_app.h`.
2. Extract AI logic into `ai_app.h` and `ai_app.cc`.
3. Extract Video logic into `video_app.h` and `video_app.cc`.
4. Refactor `dogepetv2.cc` to implement the App Host routing.
