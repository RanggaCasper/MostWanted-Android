// The options menu's update check: MostWantedActivity asks GitHub and shows the answer in a dialog of its own
#include <jni.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

namespace host {

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

}  // namespace host
