#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A replay saved as a video (src/game/replay.cpp asks, native/vulkan.cpp hands the frames, native/video.cpp encodes):
// H.264 in an MP4 in the phone's Movies/MostWanted, shown in its gallery. A PC build has none
namespace video {

bool Available();
// opens the encoder and the file for frames of w x h (multiples of 16) at fps, with the sound under them from frame 0
// on (stereo 16 bit at pcm_rate; none if empty, then a silent video); false if it cannot
bool Start(uint32_t w, uint32_t h, uint32_t fps, std::vector<int16_t> pcm = {}, uint32_t pcm_rate = 0);
bool Capturing();       // started and not stopping: native/vulkan.cpp reads each presented frame back
bool Stopping();        // stopped by the replay: native/vulkan.cpp hands what it still has, then Finish
void Size(uint32_t& w, uint32_t& h);
// a frame, BGRA rows of w * 4 bytes, the n-th since Start; any thread, copied
void Frame(const uint8_t* bgra, uint32_t w, uint32_t h, uint64_t n);
void Stop();    // the replay's: no more frames are asked for
void Finish();  // the renderer's, once the last frame is handed: the file is closed on the encoder's thread
bool Busy();    // started and not finished writing yet
std::string Result();  // where the last one went, or why it did not, once it is done; empty before

}  // namespace video
