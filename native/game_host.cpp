// the app in the background: the game waits for it to come back
#include "app/app.h"
#include "audio/output.h"
#include "common/options.h"
#include "core/log.h"
#include "host.h"

namespace game {

// in the background Android takes the window away: the game thread waits there
void WaitWhileUnfocused() {
    if (host::Foreground()) return;
    LOGI("focus lost: paused");
    g_focus_paused = true;
    audio::SetPaused(true);
    host::WaitForeground();
    audio::SetPaused(false);
    g_focus_pauses++;
    g_focus_paused = false;
    LOGI("focus back: resumed");
}

}  // namespace game
