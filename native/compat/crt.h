#pragma once
// included before every host source by CMake: the SDK's kernel/memory.cpp calls ftruncate, which glibc's headers bring
// along and bionic's do not
#include <unistd.h>

// memfd_create is in bionic from Android 11 (API 30); Android 10's kernels have the call, so a build for API 29 makes
// it itself, for the guest memory kernel/memory.cpp maps twice
#if defined(__ANDROID__) && __ANDROID_API__ < 30
#include <sys/syscall.h>
inline int memfd_create(const char* name, unsigned int flags) { return int(syscall(__NR_memfd_create, name, flags)); }
#endif
