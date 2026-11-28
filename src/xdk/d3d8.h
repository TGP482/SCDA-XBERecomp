#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// ambient occlusion over the scene drawn so far; m: its D3D perspective projection; gtao: GTAO, else SSAO
void Occlusion(const float* m, bool gtao);
// linear distance fog the next Occlusion fades with, for titles whose fog is fixed function
void LatchDistanceFog(float start, float end);
// Chaos Theory's soft shadows: the scene's shadow alpha filtered, m its D3D perspective projection; ApplyShadows
// writes the result over what the game's own edge blur left
void FilterShadows(const float* m);
void ApplyShadows();
// draws from here on are magnified about the viewport's centre, 1 for none
void ViewScale(float xy);
// TAA and FSR over the frame's view, from its first draw to before its post effects; its WorldToScreen and
// CameraToScreen, rows of 4
void SceneBegin(const float* world_to_screen, const float* camera_to_screen);
void SceneEnd();
// the view's post effects done, before its HUD
void HudBegin();

}  // namespace d3d
