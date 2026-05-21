#include "config.h"
#include "mjpeg_player.h"

// Instantiate the standalone MJPEG player
MjpegPlayer player;

// Button debounce timing
unsigned long lastDebounceTimeA = 0;
unsigned long lastDebounceTimeB = 0;
unsigned long lastDebounceTimeBoot = 0;
const unsigned long DEBOUNCE_DELAY = 50; // milliseconds

bool lastStateA = HIGH;
bool lastStateB = HIGH;
bool lastStateBoot = HIGH;

// State tracking for the status LED
unsigned long lastBlinkTime = 0;
bool ledState = false;

// Theme Colors (RGB565 format)
#define COLOR_DEEP_NAVY      0x08A5  // Splash & normal startup screen
#define COLOR_DARK_CHARCOAL  0x18C5  // Paused screen background
#define COLOR_ERROR_RED      0x9000  // SD card mount / folder error screen
#define COLOR_SUCCESS_GREEN  0x03E0  // Playback launch indicator

void setup() {
    // 1. Initialize Serial communication for diagnostics
    Serial.begin(115200);
    delay(1000); // Wait for USB Serial monitor to connect
    Serial.println("\n====================================");
    Serial.println("DogePet V2 Standalone MJPEG Player");
    Serial.println("====================================");

    // 2. Initialize GPIOs
    pinMode(BUILTIN_LED_GPIO, OUTPUT);
    digitalWrite(BUILTIN_LED_GPIO, LOW);

    pinMode(BUTTON_A_GPIO, INPUT_PULLUP);
    pinMode(BUTTON_B_GPIO, INPUT_PULLUP);
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

    // 3. Initialize LCD Display Panel
    if (!player.initDisplay()) {
        Serial.println("[ERROR] Display initialization failed!");
        while (1) {
            // Fast flash Built-in LED on fatal display error
            digitalWrite(BUILTIN_LED_GPIO, !digitalRead(BUILTIN_LED_GPIO));
            delay(100);
        }
    }
    Serial.println("[OK] Display initialized.");

    // 4. Initialize LCD Backlight
    pinMode(DISPLAY_BACKLIGHT_PIN, OUTPUT);
    digitalWrite(DISPLAY_BACKLIGHT_PIN, HIGH); // Turn backlight on
    Serial.println("[OK] Backlight active.");

    // Fill screen with deep navy blue for splash
    player.clearScreen(COLOR_DEEP_NAVY);

    // 5. Mount SD Card
    Serial.println("Mounting SD card...");
    if (!player.mountSdCard()) {
        Serial.println("[ERROR] SD card mount failed!");
        player.clearScreen(COLOR_ERROR_RED);
        
        // Loop and wait until SD card is mounted
        while (!player.mountSdCard()) {
            Serial.println("Retrying SD card mount in 2 seconds...");
            // Blink LED rapidly to indicate error state
            for (int i = 0; i < 10; i++) {
                digitalWrite(BUILTIN_LED_GPIO, HIGH);
                delay(100);
                digitalWrite(BUILTIN_LED_GPIO, LOW);
                delay(100);
            }
        }
    }
    Serial.println("[OK] SD Card mounted.");

    // 6. Start Playback
    player.clearScreen(COLOR_SUCCESS_GREEN);
    delay(500); // Brief visual confirmation
    player.clearScreen(0x0000); // Clear to black for video

    Serial.println("Starting player...");
    if (!player.start()) {
        Serial.println("[ERROR] Could not start playback (Check for .mjpeg files in SD root!)");
        player.clearScreen(COLOR_ERROR_RED);
        while (1) {
            // Heartbeat blink on empty playlist error
            digitalWrite(BUILTIN_LED_GPIO, HIGH);
            delay(150);
            digitalWrite(BUILTIN_LED_GPIO, LOW);
            delay(150);
            digitalWrite(BUILTIN_LED_GPIO, HIGH);
            delay(150);
            digitalWrite(BUILTIN_LED_GPIO, LOW);
            delay(750);
        }
    }

    Serial.println("[OK] Playback initiated in background task.");
}

void loop() {
    // 1. Process button inputs with debouncing
    handleButtons();

    // 2. Control status LED indicator
    handleStatusLED();

    // 3. Prevent task starvation (FreeRTOS yield)
    delay(10);
}

void handleButtons() {
    int readingA = digitalRead(BUTTON_A_GPIO);
    int readingB = digitalRead(BUTTON_B_GPIO);
    int readingBoot = digitalRead(BOOT_BUTTON_GPIO);

    // Button A: Play/Pause toggle
    if (readingA != lastStateA) {
        if ((millis() - lastDebounceTimeA) > DEBOUNCE_DELAY) {
            lastDebounceTimeA = millis();
            lastStateA = readingA;
            if (readingA == LOW) { // Pressed
                Serial.println("[Button A] Toggle Play/Pause");
                player.togglePause();
                if (player.isPaused()) {
                    player.clearScreen(COLOR_DARK_CHARCOAL);
                } else {
                    player.clearScreen(0x0000); // Back to black
                }
            }
        }
    }

    // Button B: Skip to next video
    if (readingB != lastStateB) {
        if ((millis() - lastDebounceTimeB) > DEBOUNCE_DELAY) {
            lastDebounceTimeB = millis();
            lastStateB = readingB;
            if (readingB == LOW) { // Pressed
                Serial.println("[Button B] Next video requested");
                player.next();
                player.clearScreen(0x0000); // Clear screen before next video
            }
        }
    }

    // BOOT Button: Print player diagnostics
    if (readingBoot != lastStateBoot) {
        if ((millis() - lastDebounceTimeBoot) > DEBOUNCE_DELAY) {
            lastDebounceTimeBoot = millis();
            lastStateBoot = readingBoot;
            if (readingBoot == LOW) { // Pressed
                Serial.println("====== DIAGNOSTICS ======");
                Serial.printf("Playing: %s\n", player.isPlaying() ? "Yes" : "No");
                Serial.printf("Paused: %s\n", player.isPaused() ? "Yes" : "No");
                Serial.printf("Current File Name: %s\n", player.getCurrentFileName().c_str());
                Serial.printf("Playlist size: %u files\n", player.getVideoCount());
                Serial.printf("Free Heap: %u bytes\n", esp_get_free_heap_size());
                Serial.println("=========================");
            }
        }
    }
}

void handleStatusLED() {
    unsigned long now = millis();

    if (!player.isPlaying()) {
        // Heartbeat blink if player task exited unexpectedly
        if (now - lastBlinkTime >= 1000) {
            lastBlinkTime = now;
            ledState = !ledState;
            digitalWrite(BUILTIN_LED_GPIO, ledState ? HIGH : LOW);
        }
    } else if (player.isPaused()) {
        // Solid ON when video is paused
        digitalWrite(BUILTIN_LED_GPIO, HIGH);
    } else {
        // Slow soft blink when video is playing normally (2 seconds cycle)
        if (now - lastBlinkTime >= 1000) {
            lastBlinkTime = now;
            ledState = !ledState;
            digitalWrite(BUILTIN_LED_GPIO, ledState ? HIGH : LOW);
        }
    }
}
