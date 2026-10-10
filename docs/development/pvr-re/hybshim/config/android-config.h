/* Minimal android-config.h for a hand-built glibc shim (normally from the android-headers package). */
#ifndef ANDROID_CONFIG_H
#define ANDROID_CONFIG_H
#define ANDROID_VERSION_MAJOR 16
#define ANDROID_VERSION_MINOR 0
#define ANDROID_VERSION_PATCH 0
#define ARCH_ARM64 1
#define HYBRIS_ANDROID_SDK_VERSION 36
#define HYBRIS_LINKER_DIR "/system/bin"
#define HYBRIS_DONT_PRINT_SYMBOLS_WITHOUT_HOOK 0
#define HYBRIS_ENABLE_LINKER_DEBUG_MAP 0
#endif
/* Hybris linker plugin config (normally from configure). */
#define LINKER_VERSION_DEFAULT 26
#define LINKER_NAME_DEFAULT "/system/bin/linker64"
#define LINKER_PLUGIN_DIR "/system/bin"
#define HYBRIS_LINKER_NAME_DEFAULT "/system/bin/linker64"
