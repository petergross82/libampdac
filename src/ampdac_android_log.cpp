#include <android/log.h>

#include <cstdio>
#include <cstdarg>

static const char *prio_name(int prio) {
    switch (prio) {
        case ANDROID_LOG_DEBUG: return "D";
        case ANDROID_LOG_INFO:  return "I";
        case ANDROID_LOG_WARN:  return "W";
        case ANDROID_LOG_ERROR: return "E";
        case ANDROID_LOG_FATAL: return "F";
        default:                return "V";
    }
}

extern "C" int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    std::fprintf(stderr, "ampdac[%s] %s: ", prio_name(prio), tag ? tag : "?");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    return 0;
}

extern "C" int __android_log_write(int prio, const char *tag, const char *text) {
    std::fprintf(stderr, "ampdac[%s] %s: %s\n",
                 prio_name(prio), tag ? tag : "?", text ? text : "");
    return 0;
}
