#pragma once
// DirectWrite for SHARP TEXT (dwrite_faces.cpp): one factory, and the faces of the vector fonts the game's text is
// drawn again with (font_redraw.cpp, loading_text.cpp)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>

namespace game::dwrite {

// shared by every thread; null without DirectWrite 3 (before Windows 10)
IDWriteFactory5* Factory();

// a font file built into the executable (resources.h); null when it cannot be read
Microsoft::WRL::ComPtr<IDWriteFontFace> ResourceFace(int resource);

// a font every Windows 10 and 11 has, by family and weight; null when missing
Microsoft::WRL::ComPtr<IDWriteFontFace> SystemFace(const wchar_t* family, DWRITE_FONT_WEIGHT weight);

}  // namespace game::dwrite
