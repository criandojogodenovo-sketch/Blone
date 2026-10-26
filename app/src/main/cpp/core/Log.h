#pragma once
// Minimal logging facade: logcat on Android, silent on host (unit tests).
//
// BLONE_LOG*   — engine lifecycle/diagnostics ("BloneNative").
// BLONE_IO*    — Fase 4 I/O pipeline instrumentation (hotfix D1): every
//                stage of an import/export logs here under the dedicated
//                tag "BloneIO" so a device failure can be attributed to
//                exactly one stage (SAF copy -> worker open -> parser ->
//                MeshBuilder -> drain/SceneOps).
#if defined(__ANDROID__)
#include <android/log.h>
#define BLONE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BloneNative", __VA_ARGS__)
#define BLONE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BloneNative", __VA_ARGS__)
#define BLONE_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "BloneNative", __VA_ARGS__)
#define BLONE_IOI(...) __android_log_print(ANDROID_LOG_INFO, "BloneIO", __VA_ARGS__)
#define BLONE_IOE(...) __android_log_print(ANDROID_LOG_ERROR, "BloneIO", __VA_ARGS__)
#else
#define BLONE_LOGI(...) ((void)0)
#define BLONE_LOGE(...) ((void)0)
#define BLONE_LOGW(...) ((void)0)
#define BLONE_IOI(...) ((void)0)
#define BLONE_IOE(...) ((void)0)
#endif
