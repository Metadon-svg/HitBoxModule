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
static bool    g_HitSoundEnabled = true;
static long    g_LastHitTime = 0;
static volatile bool g_HitDetected = false;

// ============================================================
// JNI
// ============================================================
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

// ============================================================
// Эта функция вызывается из asm stub. Только ставит флаг!
// ============================================================
extern "C" __attribute__((noinline)) void OnBulletHitNative(uintptr_t victim, uintptr_t attacker) {
    if (!attacker) return;
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) return;
    uintptr_t localPlayer = *reinterpret_cast<uintptr_t*>(base + OFFSET_LOCAL_PLAYER_PTR);
    if (localPlayer && attacker == localPlayer) {
        g_HitDetected = true;
    }
}

// ============================================================
// INLINE HOOK
// ============================================================
static void InstallBulletHitHook() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) {
        LOGE("Failed to get base address of %s", libName);
        return;
    }
    LOGI("Base: 0x%llX", (unsigned long long)base);

    uintptr_t target = base + OFFSET_BULLET_HIT_FUNC;
    uint32_t* orig = reinterpret_cast<uint32_t*>(target);

    uint32_t orig0 = orig[0];
    uint32_t orig1 = orig[1];
    uint32_t orig2 = orig[2];
    uint32_t orig3 = orig[3];

    LOGI("Orig: %08X %08X %08X %08X", orig0, orig1, orig2, orig3);

    if (orig0 != 0xD10283FF) {
        LOGW("Unexpected insn: 0x%08X", orig0);
    }

    size_t stubSize = 512;
    void* stub = mmap(nullptr, stubSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) {
        LOGE("mmap failed");
        return;
    }

    uint8_t* code = reinterpret_cast<uint8_t*>(stub);
    size_t off = 0;

    // --- helper: write uint32_t ---
    auto W32 = [&](uint32_t v) {
        memcpy(code + off, &v, 4);
        off += 4;
    };

    // --- helper: write uint64_t ---
    auto W64 = [&](uint64_t v) {
        memcpy(code + off, &v, 8);
        off += 8;
    };

    // Stub layout:
    // 1. Save regs: stp x29,x30,[sp,#-16]!
    // 2. Save x0-x7 (caller-saved + args) on stack
    // 3. mov x0, x19; mov x1, x22
    // 4. bl &OnBulletHitNative
    // 5. Restore x0-x7
    // 6. ldp x29,x30,[sp],#16
    // 7. Original 4 instructions
    // 8. br back

    // 1. Save fp/lr
    W32(0xA9BF7BFD); // stp x29, x30, [sp, #-16]!

    // 2. Save x0-x7 (64 bytes) below fp/lr
    W32(0xA90007E0); // stp x0, x1, [sp, #-64]!
    W32(0xA9010FE2); // stp x2, x3, [sp, #0x10]
    W32(0xA90217E4); // stp x4, x5, [sp, #0x20]
    W32(0xA9031FE6); // stp x6, x7, [sp, #0x30]

    // 3. Args for C++ func
    W32(0xAA1303E0); // mov x0, x19
    W32(0xAA1603E1); // mov x1, x22

    // 4. bl &OnBulletHitNative
    //    adrp x16, page(fn); add x16, x16, offset; blr x16
    uintptr_t pc_after_adrp = reinterpret_cast<uintptr_t>(code + off + 8);
    uintptr_t fn = reinterpret_cast<uintptr_t>(&OnBulletHitNative);

    uint32_t adrp = 0x90000010; // adrp x16, ...
    int64_t imm = ((fn & ~0xFFFULL) - (pc_after_adrp & ~0xFFFULL)) >> 12;
    uint32_t immlo = imm & 3;
    uint32_t immhi = (imm >> 2) & 0x7FFFF;
    adrp |= (immlo << 29);
    adrp |= (immhi << 5);
    W32(adrp);

    uint32_t addi = 0x91000210; // add x16, x16, #0
    addi |= (fn & 0xFFF) << 10;
    W32(addi);

    W32(0xD63F0200); // blr x16

    // 5. Restore x0-x7
    W32(0xA9431FE6); // ldp x6, x7, [sp, #0x30]
    W32(0xA94217E4); // ldp x4, x5, [sp, #0x20]
    W32(0xA9410FE2); // ldp x2, x3, [sp, #0x10]
    W32(0xA94007E0); // ldp x0, x1, [sp], #64

    // 6. Restore fp/lr
    W32(0xA8C17BFD); // ldp x29, x30, [sp], #16

    // 7. Original 4 instructions
    W32(orig0);
    W32(orig1);
    W32(orig2);
    W32(orig3);

    // 8. br back to original + 16
    uintptr_t retAddr = target + 16;
    pc_after_adrp = reinterpret_cast<uintptr_t>(code + off + 8);

    adrp = 0x90000010;
    imm = ((retAddr & ~0xFFFULL) - (pc_after_adrp & ~0xFFFULL)) >> 12;
    immlo = imm & 3;
    immhi = (imm >> 2) & 0x7FFFF;
    adrp |= (immlo << 29);
    adrp |= (immhi << 5);
    W32(adrp);

    addi = 0x91000210;
    addi |= (retAddr & 0xFFF) << 10;
    W32(addi);

    W32(0xD61F0200); // br x16

    // --- Patch original ---
    uintptr_t page = target & ~0xFFFULL;
    mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);

    orig[0] = 0x58000050; // ldr x16, [pc, #8]
    orig[1] = 0xD61F0210; // br x16
    orig[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stub));
    orig[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stub) >> 32);

    __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(stub), reinterpret_cast<char*>(stub) + stubSize);

    LOGI("Hook OK: target=0x%llX stub=0x%llX", (unsigned long long)target, (unsigned long long)stub);
}

void* main_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(libName));
    usleep(500 * 1000);
    LOGI("Installing hook...");
    InstallBulletHitHook();

    while (true) {
        usleep(16 * 1000);
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
