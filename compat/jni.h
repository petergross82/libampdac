#pragma once

// Minimal JNI stand-in so Android-only types in dsd_playback_manager.h
// compile on Linux. Linux ampdac never attaches a JVM.

#include <cstdint>

using jint = int32_t;
using jlong = int64_t;
using jfloat = float;
using jboolean = uint8_t;
using jsize = int32_t;
using jmethodID = void *;
using jobject = void *;
using jclass = void *;
using jstring = void *;

constexpr int JNI_OK = 0;
constexpr int JNI_ERR = -1;
constexpr int JNI_EDETACHED = -2;
constexpr int JNI_VERSION_1_6 = 0x00010006;
constexpr int JNI_TRUE = 1;
constexpr int JNI_FALSE = 0;

struct JNIEnv_;
struct JavaVM_;
using JNIEnv = JNIEnv_;
using JavaVM = JavaVM_;

struct JNIEnv_ {
    jobject NewGlobalRef(jobject obj) { return obj; }
    void DeleteGlobalRef(jobject) {}
    void CallVoidMethod(jobject, jmethodID, ...) {}
    bool ExceptionCheck() { return false; }
    void ExceptionDescribe() {}
    void ExceptionClear() {}
};

struct JavaVM_ {
    int GetEnv(void **env, int) {
        if (env) *env = nullptr;
        return JNI_EDETACHED;
    }
    int AttachCurrentThreadAsDaemon(JNIEnv **env, void *) {
        if (env) *env = nullptr;
        return JNI_ERR;
    }
    int DetachCurrentThread() { return JNI_OK; }
};
