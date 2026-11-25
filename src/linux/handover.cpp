// Linux: the relaunch handover, as app/handover.cpp has it with a named event on Windows. The old instance waits in
// kernel::Relaunch, its window still up, on a pipe whose write end this one finds in SCDA_HANDOVER_FD; a byte, or
// this process ending, lets it go.
#include <unistd.h>

#include <atomic>
#include <cstdlib>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {
int g_ready = -1;
std::atomic<bool> g_handing{false};
}  // namespace

bool StartHandover() {
    const char* fd = std::getenv("SCDA_HANDOVER_FD");
    if (!fd || !*fd) return false;
    g_ready = std::atoi(fd);
    unsetenv("SCDA_HANDOVER_FD");  // not for a later relaunch
    g_handing = g_ready >= 0;
    if (g_handing) LOGI("handover: the previous instance waits for the first frames");
    return g_handing;
}

bool HandingOver() { return g_handing; }

void FinishHandover() {
    if (!g_handing.exchange(false)) return;
    OpenGameWindow(false);
    char b = 1;
    [[maybe_unused]] ssize_t n = write(g_ready, &b, 1);
    close(g_ready);
    g_ready = -1;
}

}  // namespace app
