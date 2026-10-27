#pragma once
// Android on ARM: the SDK's runtime/cpu/dispatch.cpp includes this on every host that is not Windows; it uses none
// of it there (the game's timer runs on std::chrono)
