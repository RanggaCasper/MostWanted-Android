// The options menu's update check: on Android MostWantedActivity asks GitHub and shows the answer in a dialog of its
// own; a PC build is built from source, so the releases page opens in the browser
#ifdef __ANDROID__
#include <jni.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

namespace host {

#ifdef __ANDROID__
// a no-argument void method of the activity; any thread, SDL attaches it to the VM
static void CallActivity(const char* name) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, name, "()V");
    if (m) env->CallVoidMethod(activity, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

void CheckForUpdates() { CallActivity("checkForUpdates"); }
#else
void CheckForUpdates() { SDL_OpenURL("https://github.com/RanggaCasper/MostWanted-Android/releases/latest"); }
#endif

}  // namespace host
