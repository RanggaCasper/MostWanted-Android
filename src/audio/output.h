#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 6;

// channel order of the frames the mixer renders
enum Speaker : uint32_t {
    FrontLeft,
    FrontRight,
    Centre,
    Sub,
    BackLeft,
    BackRight,
};

// starts the platform output, which pulls interleaved float frames of kChannels from render.
// Returns once the device is open and IsSurround is known.
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// the mix is 5.1: by the audio output setting, or on automatic when the default device has 5.1 speakers
bool IsSurround();
// the audio output setting is mono: the stereo mix plays as one channel on the front pair
bool IsMono();

// audio output setting, any thread; a change applies at once
constexpr uint32_t kOutputAuto = 0;
constexpr uint32_t kOutputStereo = 1;
constexpr uint32_t kOutputSurround = 2;
constexpr uint32_t kOutputMono = 3;
void SetOutputMode(uint32_t mode);
// any thread: paused, the output stops and the mixer is not run, so every voice and stream keeps its place
void SetPaused(bool paused);

}  // namespace audio
