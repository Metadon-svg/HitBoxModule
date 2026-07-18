#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <jni.h>
#include <dlfcn.h>
#include <cstring>
#include <android/log.h>

#include "Utils.h"

#define LOG_TAG "HitBox"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define libName "libblackrussia-client.so"

// ============================================================
// АДРЕСА ХИТБОКСОВ (arm64)
// ============================================================
constexpr uintptr_t OFFSET_HITBOX_BASE = 0x2132BF0;
constexpr uintptr_t HITBOX_STEP = 0x20;

enum HitBoxPart {
    HEAD = 0, TORSO_1 = 1, TORSO_2 = 2, MID = 3,
    LEFTARM = 4, RIGHTARM = 5, LEFTLEG_1 = 6,
    RIGHTLEG_1 = 7, LEFTLEG_2 = 8, RIGHTLEG_2 = 9,
    HITBOX_COUNT = 10
};

static const float DEFAULT_VALUES[HITBOX_COUNT] = {
    0.15f, 0.20f, 0.25f, 0.25f, 0.16f,
    0.16f, 0.20f, 0.20f, 0.15f, 0.15f
};

// ============================================================
// АДРЕС sendChatMessage
// ============================================================
constexpr uintptr_t OFFSET_SEND_CHAT_MESSAGE = 0x019cfe34;

// ============================================================
// СОСТОЯНИЕ
// ============================================================
static JavaVM* g_JavaVM = nullptr;
static bool g_HitBoxEnabled = false;
static float g_HitBoxMultiplier = 1.0f;
static void* g_OrigSendChatMessage = nullptr;

// ============================================================
// Запись хитбоксов
// ============================================================
static void ApplyHitBoxes() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) { LOGE("Failed to get base address"); return; }

    uintptr_t hitboxBase = base + OFFSET_HITBOX_BASE;
    for (int i = 0; i < HITBOX_COUNT; i++) {
        uintptr_t addr = hitboxBase + (i * HITBOX_STEP);
        float value = DEFAULT_VALUES[i] * g_HitBoxMultiplier;
        Utils::WriteMemory<float>(addr, value);
    }
    LOGI("HitBoxes applied: multiplier=%.2f", g_HitBoxMultiplier);
}

static void ResetHitBoxes() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) return;

    uintptr_t hitboxBase = base + OFFSET_HITBOX_BASE;
    for (int i = 0; i < HITBOX_COUNT; i++) {
        uintptr_t addr = hitboxBase + (i * HITBOX_STEP);
        Utils::WriteMemory<float>(addr, DEFAULT_VALUES[i]);
    }
    LOGI("HitBoxes reset to default");
}

// ============================================================
// Отправка сообщения в локальный чат
// ============================================================
static void SendLocalChat(const char* message, const char* colorCode) {
    if (!g_JavaVM) { LOGE("JavaVM is null"); return; }

    JNIEnv* env = nullptr;
    if (g_JavaVM->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGE("AttachCurrentThread failed");
        return;
    }

    do {
        jclass atCls = env->FindClass("android/app/ActivityThread");
        if (!atCls) { LOGE("FindClass ActivityThread failed"); break; }
        jmethodID curAT = env->GetStaticMethodID(atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
        if (!curAT) { LOGE("GetStaticMethodID currentActivityThread failed"); break; }
        jobject at = env->CallStaticObjectMethod(atCls, curAT);
        if (!at) { LOGE("currentActivityThread returned null"); break; }
        jmethodID getApp = env->GetMethodID(atCls, "getApplication", "()Landroid/app/Application;");
        if (!getApp) { LOGE("GetMethodID getApplication failed"); break; }
        jobject ctx = env->CallObjectMethod(at, getApp);
        if (!ctx) { LOGE("getApplication returned null"); break; }

        jclass jniLibCls = env->FindClass("com/blackhub/bronline/game/core/JNILib");
        if (!jniLibCls) {
            jniLibCls = env->FindClass("com/bronline/game/core/JNILib");
        }
        if (!jniLibCls) { LOGE("FindClass JNILib failed"); break; }

        jmethodID sendChat = env->GetStaticMethodID(jniLibCls, "sendChatMessage", "(Ljava/lang/String;)V");
        if (!sendChat) { LOGE("GetStaticMethodID sendChatMessage failed"); break; }

        char coloredMsg[256];
        snprintf(coloredMsg, sizeof(coloredMsg), "%s%s", colorCode, message);

        jstring jmsg = env->NewStringUTF(coloredMsg);
        env->CallStaticVoidMethod(jniLibCls, sendChat, jmsg);
        env->DeleteLocalRef(jmsg);

        env->DeleteLocalRef(ctx);
        env->DeleteLocalRef(at);
    } while (0);

    g_JavaVM->DetachCurrentThread();
}

// ============================================================
// Парсинг команд
// ============================================================
static bool ProcessCommand(const char* text) {
    if (!text || text[0] != '/') return false;

    // /hb - тоггл хитбоксов
    if (strcmp(text, "/hb") == 0) {
        g_HitBoxEnabled = !g_HitBoxEnabled;
        if (g_HitBoxEnabled) {
            ApplyHitBoxes();
            SendLocalChat("[NONERAI] Хитбоксы включены", "{00FF00}");
        } else {
            ResetHitBoxes();
            SendLocalChat("[NONERAI] Хитбоксы выключены", "{FF0000}");
        }
        return true;
    }

    // /sethb <число>
    if (strncmp(text, "/sethb ", 7) == 0) {
        float mult = atof(text + 7);
        if (mult < 0.1f) mult = 0.1f;
        if (mult > 50.0f) mult = 50.0f;

        g_HitBoxMultiplier = mult;
        if (g_HitBoxEnabled) {
            ApplyHitBoxes();
        }

        char msg[256];
        snprintf(msg, sizeof(msg), "[NONERAI] Хитбоксы увеличены в %.1fx", mult);
        SendLocalChat(msg, "{FFFF00}");
        return true;
    }

    return false;
}

// ============================================================
// Dobby Hook
// ============================================================
typedef void (*SendChatMessage_t)(JNIEnv*, jobject, jstring);

void HookedSendChatMessage(JNIEnv* env, jobject thiz, jstring msg) {
    // Получаем JavaVM из env при первом вызове
    if (!g_JavaVM) {
        env->GetJavaVM(&g_JavaVM);
    }

    if (!msg) {
        ((SendChatMessage_t)g_OrigSendChatMessage)(env, thiz, msg);
        return;
    }

    const char* text = env->GetStringUTFChars(msg, nullptr);
    if (!text) {
        ((SendChatMessage_t)g_OrigSendChatMessage)(env, thiz, msg);
        return;
    }

    bool isCommand = ProcessCommand(text);
    env->ReleaseStringUTFChars(msg, text);

    if (isCommand) {
        return; // Не отправляем команду в чат
    }

    ((SendChatMessage_t)g_OrigSendChatMessage)(env, thiz, msg);
}

static void InstallChatHook() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) {
        LOGE("Failed to get base address of %s", libName);
        return;
    }

    uintptr_t target = base + OFFSET_SEND_CHAT_MESSAGE;
    LOGI("Hooking sendChatMessage at 0x%llX", (unsigned long long)target);

    int result = DobbyHook((void*)target, (void*)HookedSendChatMessage, &g_OrigSendChatMessage);
    if (result == 0) {
        LOGI("DobbyHook installed successfully");
    } else {
        LOGE("DobbyHook failed with code: %d", result);
    }
}

// ============================================================
// Entry Point
// ============================================================
void* main_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(libName));
    usleep(500 * 1000);
    LOGI("Library loaded, installing hook...");
    InstallChatHook();

    while (true) { sleep(1); }

    pthread_exit(nullptr);
    return nullptr;
}

__attribute__((constructor))
void _init() {
    pthread_t ptid;
    pthread_create(&ptid, nullptr, main_thread, nullptr);
}
