#pragma once
#include <jni.h>

namespace veins {

// RAII guard for JNI thread attachment.
//
// On construction: calls GetEnv to check if the current OS thread is already
// attached to the JVM.  If it is, env is populated and no attach is performed.
// If it is not (JNI_EDETACHED), AttachCurrentThread is called and the guard
// records that it attached.
//
// On destruction: calls DetachCurrentThread only if this guard performed the
// attach, preventing double-detach when the thread was already attached.
//
// Usage:
//   JNIEnvGuard g(jvm);
//   if (!g.valid()) return;          // JVM unavailable
//   g.env->CallVoidMethod(...);      // use g.env normally
//   // guard destructs → auto-detach if we attached
struct JNIEnvGuard {
    JNIEnv* env = nullptr;

    explicit JNIEnvGuard(JavaVM* vm) : jvm_(vm) {
        if (!vm) return;
        jint res = vm->GetEnv((void**)&env, JNI_VERSION_1_8);
        if (res == JNI_EDETACHED) {
            if (vm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK)
                env = nullptr;
            else
                attached_ = true;
        }
    }

    ~JNIEnvGuard() {
        if (attached_ && jvm_) jvm_->DetachCurrentThread();
    }

    bool valid() const { return env != nullptr; }

    JNIEnvGuard(const JNIEnvGuard&) = delete;
    JNIEnvGuard& operator=(const JNIEnvGuard&) = delete;

private:
    JavaVM* jvm_     = nullptr;
    bool    attached_ = false;
};

} // namespace veins
