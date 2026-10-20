#pragma once
// Minimal logging facade: logcat on Android, silent on host (unit tests).
#if defined(__ANDROID__)
#include <android/log.h>
#define BLONE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BloneNative", __VA_ARGS__)
#define BLONE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BloneNative", __VA_ARGS__)
#define BLONE_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "BloneNative", __VA_ARGS__)
// TEMPORARY diagnostic channel (bug hunt: viewport/selection/gizmo). Tag
// matches the Kotlin side (BLONE_SELECTION) so Logcat can be filtered as one
// stream. Silent on host — the unit-test build never links liblog.
#define BLONE_LOGSEL(...) __android_log_print(ANDROID_LOG_DEBUG, "BLONE_SELECTION", __VA_ARGS__)
#else
#define BLONE_LOGI(...) ((void)0)
#define BLONE_LOGE(...) ((void)0)
#define BLONE_LOGW(...) ((void)0)
#define BLONE_LOGSEL(...) ((void)0)
#endif
