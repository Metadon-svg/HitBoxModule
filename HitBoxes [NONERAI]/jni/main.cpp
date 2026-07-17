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
    if (!InitJNI()) return;

    JNIEnv* env = nullptr;
    if (g_JavaVM->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;

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

        jstring jpath = env->NewStringUTF(HIT_SOUND_PATH);
        jclass uriCls = env->FindClass("android/net/Uri");
        jmethodID parse = env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        jobject uri = env->CallStaticObjectMethod(uriCls, parse, jpath);
        jclass mpCls = env->FindClass("android/media/MediaPlayer");
        jmethodID create = env->GetStaticMethodID(mpCls, "create", "(Landroid/content/Context;Landroid/net/Uri;)Landroid/media/MediaPlayer;");
        jobject mp = env->CallStaticObjectMethod(mpCls, create, ctx, uri);

        if (mp) {
            jmethodID start = env->GetMethodID(mpCls, "start", "()V");
            env->CallVoidMethod(mp, start);
            jmethodID release = env->GetMethodID(mpCls, "release", "()V");
            env->CallVoidMethod(mp, release);
            env->DeleteLocalRef(mp);
        }

        env->DeleteLocalRef(jpath);
        env->DeleteLocalRef(uri);
        env->DeleteLocalRef(ctx);
        env->DeleteLocalRef(at);
    } while (0);

    g_JavaVM->DetachCurrentThread();
}

extern "C" void OnBulletHitNative(uintptr_t victim, uintptr_t attacker) {
    if (!attacker) return;
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) return;
    uintptr_t localPlayer = *reinterpret_cast<uintptr_t*>(base + OFFSET_LOCAL_PLAYER_PTR);
    if (localPlayer && attacker == localPlayer) {
        PlayHitSound();
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

static void InstallBulletHitHook() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) {
        LOGE("Failed to get base address of %s", libName);
        return;
    }

    uintptr_t target = base + OFFSET_BULLET_HIT_FUNC;
    uint32_t* p = reinterpret_cast<uint32_t*>(target);

    g_OrigInsn[0] = p[0];
    g_OrigInsn[1] = p[1];
    g_OrigInsn[2] = p[2];
    g_OrigInsn[3] = p[3];

    if (g_OrigInsn[0] != 0xD10283FF) {
        LOGW("Unexpected first instruction at target: 0x%08X", g_OrigInsn[0]);
    }

    size_t stubSize = 256;
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

    auto emitAdrpAddBr = [&](uintptr_t insnAddr, uintptr_t targetAddr) {
        s[idx++] = MakeAdrp(16, insnAddr, targetAddr);
        s[idx++] = MakeAdd(16, 16, targetAddr & 0xFFF);
        s[idx++] = 0xD61F0210; // br x16
    };

    s[idx++] = 0xA9BF7BFD; // stp x29, x30, [sp, #-16]!
    s[idx++] = 0xAA1303E0; // mov x0, x19
    s[idx++] = 0xAA1603E1; // mov x1, x22

    emitAdrpAddBr(stub + idx * 4, fnAddr);

    s[idx++] = 0xA8C17BFD; // ldp x29, x30, [sp], #16

    s[idx++] = g_OrigInsn[0];
    s[idx++] = g_OrigInsn[1];
    s[idx++] = g_OrigInsn[2];
    s[idx++] = g_OrigInsn[3];

    emitAdrpAddBr(stub + idx * 4, retAddr);

    uintptr_t page = target & ~0xFFFULL;
    mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);

    p[0] = 0x58000050; // ldr x16, [pc, #8]
    p[1] = 0xD61F0210; // br x16
    p[2] = static_cast<uint32_t>(stub);
    p[3] = static_cast<uint32_t>(stub >> 32);

    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target) + 16);

    LOGI("Hook installed at 0x%llX -> trampoline 0x%llX",
         (unsigned long long)target, (unsigned long long)stub);
}

void* main_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(libName));
    usleep(500 * 1000);
    InstallBulletHitHook();
    pthread_exit(nullptr);
    return nullptr;
}

__attribute__((constructor))
void _init() {
    pthread_t ptid;
    pthread_create(&ptid, nullptr, main_thread, nullptr);
}
