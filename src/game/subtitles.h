#pragma once
#include <cstdint>
// The movies' subtitles (subtitles.cpp): cues read when a Bink opens, drawn in the game's font over the frame shown
struct X86Context;

namespace game::subtitles {

// the front end player's frame (sub_00038F96, esi the player) has drawn the movie's picture: the cue showing at its
// Bink's frame is put up for the ImGui pass, from the tile hook in loading.cpp
void AfterMoviePicture(X86Context& ctx, uint8_t* base);

// a movie on a Magma page (the Cinematics menu, the training videos) has drawn its frame, from the movie draw hooks
// in movie_aspect.cpp: its cue goes near the bottom of its picture
void AfterPageMovie(uint32_t bink);

// a movie picture's quad, as fractions of the viewport, from the quad setup hook in hud.cpp
void MoviePicture(float x0, float y0, float x1, float y1);

}  // namespace game::subtitles
