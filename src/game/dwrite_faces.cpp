#include "game/dwrite_faces.h"

#include <mutex>

#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace game::dwrite {

namespace {
std::mutex g_lock;
ComPtr<IDWriteFactory5> g_factory;
ComPtr<IDWriteInMemoryFontFileLoader> g_loader;
bool g_tried = false;
}  // namespace

IDWriteFactory5* Factory() {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_tried) {
        g_tried = true;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                       reinterpret_cast<IUnknown**>(g_factory.GetAddressOf()))))
            LOGW("fonts: DirectWrite 3 is missing, no ClearType text");
    }
    return g_factory.Get();
}

ComPtr<IDWriteFontFace> ResourceFace(int resource) {
    ComPtr<IDWriteFontFace> face;
    IDWriteFactory5* factory = Factory();
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(resource), RT_RCDATA);
    HGLOBAL loaded = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!factory || !data) return face;
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_loader && SUCCEEDED(factory->CreateInMemoryFontFileLoader(&g_loader)))
        factory->RegisterFontFileLoader(g_loader.Get());
    ComPtr<IDWriteFontFile> file;
    // no owner: the loader keeps a copy
    if (g_loader && SUCCEEDED(g_loader->CreateInMemoryFontFileReference(factory, data, SizeofResource(nullptr, res),
                                                                        nullptr, &file)))
        factory->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, file.GetAddressOf(), 0, DWRITE_FONT_SIMULATIONS_NONE,
                                &face);
    return face;
}

ComPtr<IDWriteFontFace> SystemFace(const wchar_t* family, DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteFontFace> face;
    IDWriteFactory5* factory = Factory();
    ComPtr<IDWriteFontCollection> fonts;
    ComPtr<IDWriteFontFamily> found;
    ComPtr<IDWriteFont> font;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (factory && SUCCEEDED(factory->GetSystemFontCollection(&fonts)) &&
        SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists &&
        SUCCEEDED(fonts->GetFontFamily(index, &found)) &&
        SUCCEEDED(found->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
        font->CreateFontFace(&face);
    return face;
}

}  // namespace game::dwrite
