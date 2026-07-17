#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <jni.h>
#include <dlfcn.h>
#include <ctime>
#include <android/log.h>

#include "Utils.h"

#define LOG_TAG "HitSound"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define libName "libblackrussia-client.so"

constexpr uintptr_t OFFSET_BULLET_HIT_FUNC    = 0x01A63370;
constexpr uintptr_t OFFSET_LOCAL_PLAYER_PTR   = 0x215CF50;
constexpr const char* HIT_SOUND_PATH = "/storage/emulated/0/Nonerai/hit.mp3";
constexpr int HIT_SOUND_COOLDOWN_MS = 80;

static JavaVM* g_JavaVM = nullptr;
static void*   g_Trampoline = nullptr;
static uint32_t g_OrigInsn[4];
static bool    g_HitSoundEnabled = true;
static long    g_LastHitTime = 0;
static volatile bool g_HitDetected = false;

static bool InitJNI() {
    if (g_JavaVM) return true;
    using JNI_GetCreatedJavaVMs_t = jint (*)(JavaVM**, jsize, jsize*);
    JNI_GetCreatedJavaVMs_t fn = nullptr;
    void* h = dlopen("libnativehelper.so", RTLD_NOW);
    if (!h) h = dlopen("libandroid_runtime.so", RTLD_NOW);
    if (!h) h = dlopen("libart.so", RTLD_NOW);
    if (h) fn = (JNI_GetCreatedJavaVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
    if (!fn) fn = (JNI_GetCreatedJavaVMs_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (!fn) return false;
    jsize vmCount = 0;
    fn(&g_JavaVM, 1, &vmCount);
    return (vmCount > 0 && g_JavaVM != nullptr);
}

static void PlayHitSound() {
    if (!g_HitSoundEnabled) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long nowMs = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (nowMs - g_LastHitTime < HIT_SOUND_COOLDOWN_MS) return;
    g_LastHitTime = nowMs;

    if (!InitJNI()) { LOGE("InitJNI failed"); return; }

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

        jstring jpath = env->NewStringUTF(HIT_SOUND_PATH);
        jclass uriCls = env->FindClass("android/net/Uri");
        if (!uriCls) { LOGE("FindClass Uri failed"); break; }
        jmethodID parse = env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        if (!parse) { LOGE("GetStaticMethodID parse failed"); break; }
        jobject uri = env->CallStaticObjectMethod(uriCls, parse, jpath);
        if (!uri) { LOGE("Uri.parse returned null"); break; }

        jclass mpCls = env->FindClass("android/media/MediaPlayer");
        if (!mpCls) { LOGE("FindClass MediaPlayer failed"); break; }
        jmethodID create = env->GetStaticMethodID(mpCls, "create", "(Landroid/content/Context;Landroid/net/Uri;)Landroid/media/MediaPlayer;");
        if (!create) { LOGE("GetStaticMethodID MediaPlayer.create failed"); break; }
        jobject mp = env->CallStaticObjectMethod(mpCls, create, ctx, uri);

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("JNI Exception during MediaPlayer.create");
        }

        if (mp) {
            jmethodID start = env->GetMethodID(mpCls, "start", "()V");
            if (start) env->CallVoidMethod(mp, start);
            jmethodID release = env->GetMethodID(mpCls, "release", "()V");
            if (release) env->CallVoidMethod(mp, release);
            env->DeleteLocalRef(mp);
            LOGI("Hit sound played");
        } else {
            LOGE("MediaPlayer.create returned null");
        }

        env->DeleteLocalRef(jpath);
        env->DeleteLocalRef(uri);
        env->DeleteLocalRef(ctx);
        env->DeleteLocalRef(at);
    } while (0);

    g_JavaVM->DetachCurrentThread();
}

extern "C" __attribute__((noinline)) void OnBulletHitNative(uintptr_t victim, uintptr_t attacker) {
    if (!attacker) return;
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) return;
    uintptr_t localPlayer = *reinterpret_cast<uintptr_t*>(base + OFFSET_LOCAL_PLAYER_PTR);
    if (localPlayer && attacker == localPlayer) {
        g_HitDetected = true;
    }
}

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

static uint32_t MakeSubSp(int imm) {
    return 0xD1000000 | ((imm & 0xFFF) << 10) | (31 << 5) | 31;
}

static uint32_t MakeAddSp(int imm) {
    return 0x91000000 | ((imm & 0xFFF) << 10) | (31 << 5) | 31;
}

static uint32_t MakeStp(int rt, int rt2, int rn, int imm) {
    uint32_t insn = 0xA9000000 | (rt & 0x1F) | ((rn & 0x1F) << 5) | ((rt2 & 0x1F) << 10);
    int imm7 = imm / 8;
    insn |= (imm7 & 0x7F) << 15;
    return insn;
}

static uint32_t MakeLdp(int rt, int rt2, int rn, int imm) {
    uint32_t insn = 0xA9400000 | (rt & 0x1F) | ((rn & 0x1F) << 5) | ((rt2 & 0x1F) << 10);
    int imm7 = imm / 8;
    insn |= (imm7 & 0x7F) << 15;
    return insn;
}

static uint32_t MakeStr(int rt, int rn, int imm) {
    uint32_t insn = 0xF9000000 | (rt & 0x1F) | ((rn & 0x1F) << 5);
    int imm12 = imm / 8;
    insn |= (imm12 & 0xFFF) << 10;
    return insn;
}

static uint32_t MakeLdr(int rt, int rn, int imm) {
    uint32_t insn = 0xF9400000 | (rt & 0x1F) | ((rn & 0x1F) << 5);
    int imm12 = imm / 8;
    insn |= (imm12 & 0xFFF) << 10;
    return insn;
}

static void InstallBulletHitHook() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) {
        LOGE("Failed to get base address of %s", libName);
        return;
    }
    LOGI("Base address: 0x%llX", (unsigned long long)base);

    uintptr_t target = base + OFFSET_BULLET_HIT_FUNC;
    uint32_t* p = reinterpret_cast<uint32_t*>(target);

    g_OrigInsn[0] = p[0];
    g_OrigInsn[1] = p[1];
    g_OrigInsn[2] = p[2];
    g_OrigInsn[3] = p[3];

    LOGI("Original insn: %08X %08X %08X %08X",
         g_OrigInsn[0], g_OrigInsn[1], g_OrigInsn[2], g_OrigInsn[3]);

    if (g_OrigInsn[0] != 0xD10283FF) {
        LOGW("Unexpected first instruction: 0x%08X (expected 0xD10283FF)", g_OrigInsn[0]);
    }

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
    uintptr_t fnAddr = reinterpret_cast<uintptr_t>(&OnBulletHitNative);
    int idx = 0;

    auto emitAdrpAddBl = [&](uintptr_t insnAddr, uintptr_t targetAddr) {
        s[idx++] = MakeAdrp(16, insnAddr, targetAddr);
        s[idx++] = MakeAdd(16, 16, targetAddr & 0xFFF);
        s[idx++] = 0xD63F0210; // bl x16
    };

    auto emitAdrpAddBr = [&](uintptr_t insnAddr, uintptr_t targetAddr) {
        s[idx++] = MakeAdrp(16, insnAddr, targetAddr);
        s[idx++] = MakeAdd(16, 16, targetAddr & 0xFFF);
        s[idx++] = 0xD61F0210; // br x16
    };

    // Save all caller-saved registers (x0-x17, x29, x30) = 160 bytes
    s[idx++] = MakeSubSp(160);
    s[idx++] = MakeStp(0, 1, 31, 0);
    s[idx++] = MakeStp(2, 3, 31, 16);
    s[idx++] = MakeStp(4, 5, 31, 32);
    s[idx++] = MakeStp(6, 7, 31, 48);
    s[idx++] = MakeStp(8, 9, 31, 64);
    s[idx++] = MakeStp(10, 11, 31, 80);
    s[idx++] = MakeStp(12, 13, 31, 96);
    s[idx++] = MakeStp(14, 15, 31, 112);
    s[idx++] = MakeStp(16, 17, 31, 128);
    s[idx++] = MakeStr(29, 31, 144);
    s[idx++] = MakeStr(30, 31, 152);

    // Arguments: x0 = victim (x19), x1 = attacker (x22)
    s[idx++] = 0xAA1303E0; // mov x0, x19
    s[idx++] = 0xAA1603E1; // mov x1, x22

    // bl &OnBulletHitNative
    emitAdrpAddBl(stub + idx * 4, fnAddr);

    // Restore all registers
    s[idx++] = MakeLdp(0, 1, 31, 0);
    s[idx++] = MakeLdp(2, 3, 31, 16);
    s[idx++] = MakeLdp(4, 5, 31, 32);
    s[idx++] = MakeLdp(6, 7, 31, 48);
    s[idx++] = MakeLdp(8, 9, 31, 64);
    s[idx++] = MakeLdp(10, 11, 31, 80);
    s[idx++] = MakeLdp(12, 13, 31, 96);
    s[idx++] = MakeLdp(14, 15, 31, 112);
    s[idx++] = MakeLdp(16, 17, 31, 128);
    s[idx++] = MakeLdr(29, 31, 144);
    s[idx++] = MakeLdr(30, 31, 152);
    s[idx++] = MakeAddSp(160);

    // Original 4 instructions
    s[idx++] = g_OrigInsn[0];
    s[idx++] = g_OrigInsn[1];
    s[idx++] = g_OrigInsn[2];
    s[idx++] = g_OrigInsn[3];

    // Jump back to original
    emitAdrpAddBr(stub + idx * 4, retAddr);

    // Patch original function
    uintptr_t page = target & ~0xFFFULL;
    if (mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect failed");
        return;
    }

    p[0] = 0x58000050; // ldr x16, [pc, #8]
    p[1] = 0xD61F0210; // br x16
    p[2] = static_cast<uint32_t>(stub);
    p[3] = static_cast<uint32_t>(stub >> 32);

    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(g_Trampoline),
                            reinterpret_cast<char*>(g_Trampoline) + stubSize);

    LOGI("Hook installed at 0x%llX -> trampoline 0x%llX",
         (unsigned long long)target, (unsigned long long)stub);
}

void* main_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(libName));
    usleep(500 * 1000);
    LOGI("Library loaded, installing hook...");
    InstallBulletHitHook();

    while (true) {
        usleep(16 * 1000); // ~60 fps check
        if (g_HitDetected) {
            g_HitDetected = false;
            PlayHitSound();
        }
    }

    pthread_exit(nullptr);
    return nullptr;
}

__attribute__((constructor))
void _init() {
    pthread_t ptid;
    pthread_create(&ptid, nullptr, main_thread, nullptr);
}
