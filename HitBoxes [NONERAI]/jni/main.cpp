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

// Оригинальные инструкции sendChatMessage (4 инструкции = 16 байт)
static uint32_t g_OrigInsn[4];
static void* g_Trampoline = nullptr;

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
        if (!atCls) break;
        jmethodID curAT = env->GetStaticMethodID(atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
        if (!curAT) break;
        jobject at = env->CallStaticObjectMethod(atCls, curAT);
        if (!at) break;
        jmethodID getApp = env->GetMethodID(atCls, "getApplication", "()Landroid/app/Application;");
        if (!getApp) break;
        jobject ctx = env->CallObjectMethod(at, getApp);
        if (!ctx) break;

        jclass jniLibCls = env->FindClass("com/blackhub/bronline/game/core/JNILib");
        if (!jniLibCls) {
            jniLibCls = env->FindClass("com/bronline/game/core/JNILib");
        }
        if (!jniLibCls) break;

        jmethodID sendChat = env->GetStaticMethodID(jniLibCls, "sendChatMessage", "(Ljava/lang/String;)V");
        if (!sendChat) break;

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
// C++ функция, вызываемая из asm stub
// Принимает: x0=JNIEnv*, x1=jobject, x2=jstring
// ============================================================
extern "C" __attribute__((noinline)) void HookedSendChatMessage_C(uintptr_t env, uintptr_t thiz, uintptr_t msg) {
    JNIEnv* jniEnv = reinterpret_cast<JNIEnv*>(env);
    jstring jmsg = reinterpret_cast<jstring>(msg);

    if (!jmsg) {
        // Вызов оригинала через trampoline
        typedef void (*OrigFunc_t)(JNIEnv*, jobject, jstring);
        ((OrigFunc_t)g_Trampoline)(jniEnv, reinterpret_cast<jobject>(thiz), jmsg);
        return;
    }

    // Получаем JavaVM при первом вызове
    if (!g_JavaVM) {
        jniEnv->GetJavaVM(&g_JavaVM);
    }

    const char* text = jniEnv->GetStringUTFChars(jmsg, nullptr);
    if (!text) {
        typedef void (*OrigFunc_t)(JNIEnv*, jobject, jstring);
        ((OrigFunc_t)g_Trampoline)(jniEnv, reinterpret_cast<jobject>(thiz), jmsg);
        return;
    }

    bool isCommand = ProcessCommand(text);
    jniEnv->ReleaseStringUTFChars(jmsg, text);

    if (isCommand) {
        return; // Не отправляем команду в чат
    }

    typedef void (*OrigFunc_t)(JNIEnv*, jobject, jstring);
    ((OrigFunc_t)g_Trampoline)(jniEnv, reinterpret_cast<jobject>(thiz), jmsg);
}

// ============================================================
// ARM64 Inline Hook
// ============================================================
static uint32_t MakeAdrp(int rd, uintptr_t pc, uintptr_t target) {
    uint32_t insn = 0x90000000 | (rd & 0x1F);
    int64_t imm = ((target & ~0xFFFULL) - (pc & ~0xFFFULL)) >> 12;
    uint32_t immlo = imm & 3;
    uint32_t immhi = (imm >> 2) & 0x7FFFF;
    insn |= (immlo << 29);
    insn |= (immhi << 5);
    return insn;
}

static uint32_t MakeAdd(int rd, int rn, int imm12) {
    uint32_t insn = 0x91000000 | (rd & 0x1F) | ((rn & 0x1F) << 5);
    insn |= (imm12 & 0xFFF) << 10;
    return insn;
}

static void InstallChatHook() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) {
        LOGE("Failed to get base address of %s", libName);
        return;
    }

    uintptr_t target = base + OFFSET_SEND_CHAT_MESSAGE;
    uint32_t* p = reinterpret_cast<uint32_t*>(target);

    g_OrigInsn[0] = p[0];
    g_OrigInsn[1] = p[1];
    g_OrigInsn[2] = p[2];
    g_OrigInsn[3] = p[3];

    LOGI("Original: %08X %08X %08X %08X", g_OrigInsn[0], g_OrigInsn[1], g_OrigInsn[2], g_OrigInsn[3]);

    size_t stubSize = 512;
    g_Trampoline = mmap(nullptr, stubSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_Trampoline == MAP_FAILED) {
        LOGE("mmap failed");
        return;
    }

    uint32_t* s = reinterpret_cast<uint32_t*>(g_Trampoline);
    uintptr_t stub = reinterpret_cast<uintptr_t>(g_Trampoline);
    uintptr_t retAddr = target + 16;
    uintptr_t fnAddr = reinterpret_cast<uintptr_t>(&HookedSendChatMessage_C);
    int idx = 0;

    // Save fp/lr
    s[idx++] = 0xA9BF7BFD; // stp x29, x30, [sp, #-16]!

    // Save x0-x7 (caller-saved + args) below fp/lr
    s[idx++] = 0xA90007E0; // stp x0, x1, [sp, #-64]!
    s[idx++] = 0xA9010FE2; // stp x2, x3, [sp, #0x10]
    s[idx++] = 0xA90217E4; // stp x4, x5, [sp, #0x20]
    s[idx++] = 0xA9031FE6; // stp x6, x7, [sp, #0x30]

    // Args already in x0, x1, x2 from original call
    // bl &HookedSendChatMessage_C
    uintptr_t pc_after = stub + idx * 4 + 8;
    s[idx++] = MakeAdrp(16, pc_after, fnAddr);
    s[idx++] = MakeAdd(16, 16, fnAddr & 0xFFF);
    s[idx++] = 0xD63F0200; // blr x16

    // Restore x0-x7
    s[idx++] = 0xA9431FE6; // ldp x6, x7, [sp, #0x30]
    s[idx++] = 0xA94217E4; // ldp x4, x5, [sp, #0x20]
    s[idx++] = 0xA9410FE2; // ldp x2, x3, [sp, #0x10]
    s[idx++] = 0xA94007E0; // ldp x0, x1, [sp], #64

    // Restore fp/lr
    s[idx++] = 0xA8C17BFD; // ldp x29, x30, [sp], #16

    // Original 4 instructions
    s[idx++] = g_OrigInsn[0];
    s[idx++] = g_OrigInsn[1];
    s[idx++] = g_OrigInsn[2];
    s[idx++] = g_OrigInsn[3];

    // br back to original + 16
    pc_after = stub + idx * 4 + 8;
    s[idx++] = MakeAdrp(16, pc_after, retAddr);
    s[idx++] = MakeAdd(16, 16, retAddr & 0xFFF);
    s[idx++] = 0xD61F0200; // br x16

    // Patch original
    uintptr_t page = target & ~0xFFFULL;
    mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);

    p[0] = 0x58000050; // ldr x16, [pc, #8]
    p[1] = 0xD61F0210; // br x16
    p[2] = static_cast<uint32_t>(stub);
    p[3] = static_cast<uint32_t>(stub >> 32);

    __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(g_Trampoline), reinterpret_cast<char*>(g_Trampoline) + stubSize);

    LOGI("Hook installed: target=0x%llX stub=0x%llX", (unsigned long long)target, (unsigned long long)stub);
}

// ============================================================
// Entry Point
// ============================================================
void* main_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(libName));
    usleep(500 * 1000);
    LOGI("Installing hook...");
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
