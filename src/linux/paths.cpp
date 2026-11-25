// Linux: where the game reads and writes. Built from source, everything stays beside the executable, as on
// Windows. Run from an AppImage, whose files are read only, what the game writes (settings, logs, saves, shader
// caches, the compiled game code, the link to the game files) goes to $XDG_DATA_HOME/scda-xberecomp instead;
// SCDA_DATA_DIR picks another folder.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "app/app.h"

namespace app {

std::filesystem::path InstallDir() {
    static const std::filesystem::path dir = [] {
        char p[4096] = {};
        ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
        return std::filesystem::path(std::string(p, n > 0 ? size_t(n) : 0)).parent_path();
    }();
    return dir;
}

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d;
        if (const char* e = std::getenv("SCDA_DATA_DIR"); e && *e) {
            d = e;
        } else if (std::getenv("APPIMAGE")) {
            const char* xdg = std::getenv("XDG_DATA_HOME");
            const char* home = std::getenv("HOME");
            d = xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home ? home : ".") / ".local" / "share";
            d /= "scda-xberecomp";
        } else {
            return InstallDir();
        }
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

bool InAppImage() { return std::getenv("APPIMAGE") != nullptr; }

}  // namespace app
