#pragma once
#include <cstdint>

// Gyro aiming from any SDL controller with a gyro, apart from the game: the game owns the settings and turns its view.
namespace gyro {

// the turn about the controller's own axes, or about the real world's up whatever the controller's tilt
enum Axis : uint32_t { kYaw, kRoll, kWorld };
inline constexpr const char* kAxisNames[] = {"YAW", "ROLL", "WORLD"};

// buttons a game's own layout leaves free; gyro.cpp maps them to SDL's
inline constexpr const char* kButtonNames[] = {"NONE",           "TOUCHPAD",      "MISC",
                                               "RIGHT PADDLE 1", "LEFT PADDLE 1", "RIGHT PADDLE 2",
                                               "LEFT PADDLE 2"};

enum ButtonMode : uint32_t { kOffWhileHeld, kOnWhileHeld, kToggle };
inline constexpr const char* kButtonModeNames[] = {"OFF WHILE HELD", "ON WHILE HELD", "TOGGLE"};

struct Settings {
    bool enabled = false;
    float sensitivity = 2.0f;  // degrees the view turns a degree the controller turns
    uint32_t axis = kYaw;
    uint32_t button = 0;  // index in kButtonNames
    uint32_t button_mode = kOffWhileHeld;
    uint32_t recenter_button = 0;  // index in kButtonNames: levels the view
};

// degrees since the last call, yaw left and pitch down positive
struct Turn {
    float yaw = 0;
    float pitch = 0;
    bool recenter = false;  // the recenter button went down
};

// game thread, each look update; a turn gathered while the look did not run is dropped
Turn Take(const Settings& s);

// the drift taken from the next window the controller lies still through; the gyro turns nothing till then
void StartCalibration();
bool Calibrating();

}  // namespace gyro
