#pragma once
#include <cstdint>

namespace game {

// the audio thread's (native/audio.cpp), each block of the game's own mix (kChannels, before My Music): recorded with
// the race for its replay, and while a replay plays given the recording's sound at the replay's place instead
// (src/game/replay.cpp)
void ReplayAudio(float* mix, uint32_t frames);

}  // namespace game
