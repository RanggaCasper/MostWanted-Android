#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A replay or cinematic mode saved as a video (src/game/replay.cpp and cinematic.cpp ask, native/vulkan.cpp hands the
// frames, native/video.cpp encodes): H.264 in an MP4 in the phone's Movies/MostWanted, shown in its gallery. A PC
// build has none. And cinematic mode's photos: a PNG of the next frame, in Pictures/MostWanted (beside the executable,
// screenshots/, on a PC)
namespace video {

bool Available();
// opens the encoder and the file for frames of w x h (multiples of 16) at fps, frame n at n / fps, with the sound under
// them from frame 0 on (stereo 16 bit at pcm_rate; none if empty, then a silent video); false if it cannot
bool Start(uint32_t w, uint32_t h, uint32_t fps, std::vector<int16_t> pcm = {}, uint32_t pcm_rate = 0);
// as Start, but live: a frame at its own time as it is presented, at most fps a second (the others not read back, and
// dropped when the encoder falls behind rather than holding the game), the sound the game's as it plays (Audio)
bool StartLive(uint32_t w, uint32_t h, uint32_t fps);
bool Capturing();       // started and not stopping: native/vulkan.cpp reads each presented frame back
bool Stopping();        // stopped by the replay: native/vulkan.cpp hands what it still has, then Finish
void Size(uint32_t& w, uint32_t& h);
// w x h for a picture of the aspect about height high: multiples of 16, 1920 wide at most
void SizeFor(uint32_t height, float aspect, uint32_t& w, uint32_t& h);
// the renderer's, a frame about to be read back: its time in microseconds, or -1 when live and not due yet
int64_t Stamp();
// a frame, BGRA rows of w * 4 bytes, at us (Stamp's); any thread, copied
void Frame(const uint8_t* bgra, uint32_t w, uint32_t h, int64_t us);
// the audio thread's, each block of the mix as played (stereo floats at rate): kept while a live one captures
void Audio(const float* stereo, uint32_t frames, uint32_t rate);
double Seconds();  // live: since it started
void Stop();    // the replay's: no more frames are asked for
void Finish();  // the renderer's, once the last frame is handed: the file is closed on the encoder's thread
bool Busy();    // started and not finished writing yet
std::string Result();  // where the last one went, or why it did not, once it is done; empty before

// a photo of the next presented game frame (before the UI), at the renderer's size and widened to the aspect
void TakePhoto(float aspect);
bool PhotoWanted(float& aspect);  // the renderer's: one is asked for
void Photo(const uint8_t* bgra, uint32_t w, uint32_t h);  // the renderer's: copied, written on a thread of its own
bool PhotoBusy();                 // asked for and not written yet
std::string PhotoResult();        // the last one's file, "!" if it failed

}  // namespace video
