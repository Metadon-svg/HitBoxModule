#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <jni.h>
#include <dlfcn.h>

#include "Utils.h"

#define libName "libblackrussia-client.so"

// ============================================================
// АДРЕСА (смещения от базы библиотеки, arm64)
// Найдены через radare2 внутри .so
// ============================================================
constexpr uintptr_t OFFSET_BULLET_HIT_FUNC    = 0x01A63370;  // обработчик попадания
constexpr uintptr_t OFFSET_LOCAL_PLAYER_PTR   = 0x215CF50;   // CPlayerPed* localPlayer

// Звуковой файл (должен лежать на устройстве)
constexpr const char* HIT_SOUND_PATH = "/storage/emulated/0/Nonerai/hit.mp3";

// Кулдаун между звуками (мс)
constexpr int HIT_SOUND_COOLDOWN_MS = 80;

// ============================================================
// ГЛОБАЛЬНОЕ СОСТОЯНИЕ
// ============================================================
static JavaVM* g_JavaVM = nullptr;
static void*   g_Trampoline = nullptr;
static uint32_t g_OrigInsn[4];
static bool    g_HitSoundEnabled = true;
static long    g_LastHitTime = 0;

// ============================================================
// JNI: получение JavaVM через dlsym (без JNI_OnLoad)
// ============================================================
static bool InitJNI() {
    if (g_JavaVM) return true;

    using JNI_GetCreatedJavaVMs_t = jint (*)(JavaVM**, jsize, jsize*);
    JNI_GetCreatedJavaVMs_t fn = nullptr;

    void* h = dlopen("libnativehelper.so", RTLD_NOW);
    if (!h) h = dlopen("libandroid_runtime.so", RTLD_NOW);
    if (!h) h = dlopen("libart.so", RTLD_NOW);
    if (h) {
        fn = (JNI_GetCreatedJavaVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
    }
    if (!fn) {
        // Попробуем резолв напрямую (может быть доступен из линкера)
        fn = (JNI_GetCreatedJavaVMs_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    }
    if (!fn) return false;

    jsize vmCount = 0;
    fn(&g_JavaVM, 1, &vmCount);
    return (vmCount > 0 && g_JavaVM != nullptr);
}

// ============================================================
// Проигрывание звука через MediaPlayer (JNI)
// ============================================================
static void PlayHitSound() {
    if (!g_HitSoundEnabled) return;

    // Простой кулдаун
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long nowMs = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (nowMs - g_LastHitTime < HIT_SOUND_COOLDOWN_MS) return;
    g_LastHitTime = nowMs;

    if (!InitJNI()) return;

    JNIEnv* env = nullptr;
    jint attachResult = g_JavaVM->AttachCurrentThread(&env, nullptr);
    if (attachResult != JNI_OK || !env) return;

    // Получаем Context через ActivityThread.currentActivityThread().getApplication()
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) goto detach;
    jmethodID curAT = env->GetStaticMethodID(atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (!curAT) goto detach;
    jobject at = env->CallStaticObjectMethod(atCls, curAT);
    if (!at) goto detach;
    jmethodID getApp = env->GetMethodID(atCls, "getApplication", "()Landroid/app/Application;");
    if (!getApp) goto detach;
    jobject ctx = env->CallObjectMethod(at, getApp);
    if (!ctx) goto detach;

    // Uri.parse("file:///...")
    jstring jpath = env->NewStringUTF(HIT_SOUND_PATH);
    jclass uriCls = env->FindClass("android/net/Uri");
    jmethodID parse = env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jobject uri = env->CallStaticObjectMethod(uriCls, parse, jpath);

    // MediaPlayer.create(context, uri)
    jclass mpCls = env->FindClass("android/media/MediaPlayer");
    jmethodID create = env->GetStaticMethodID(mpCls, "create", "(Landroid/content/Context;Landroid/net/Uri;)Landroid/media/MediaPlayer;");
    jobject mp = env->CallStaticObjectMethod(mpCls, create, ctx, uri);

    if (mp) {
        jmethodID start = env->GetMethodID(mpCls, "start", "()V");
        env->CallVoidMethod(mp, start);

        // Чтобы не течь — сразу release (звук короткий, успеет стартануть)
        jmethodID release = env->GetMethodID(mpCls, "release", "()V");
        env->CallVoidMethod(mp, release);
        env->DeleteLocalRef(mp);
    }

    env->DeleteLocalRef(jpath);
    env->DeleteLocalRef(uri);
    env->DeleteLocalRef(ctx);
    env->DeleteLocalRef(at);

detach:
    g_JavaVM->DetachCurrentThread();
}

// ============================================================
// C++ обработчик попадания (вызывается из asm-stub)
// ============================================================
extern "C" void OnBulletHitNative(uintptr_t victim, uintptr_t attacker) {
    if (!attacker) return;

    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) return;

    uintptr_t localPlayer = *reinterpret_cast<uintptr_t*>(base + OFFSET_LOCAL_PLAYER_PTR);
    if (localPlayer && attacker == localPlayer) {
        PlayHitSound();
    }
}

// ============================================================
// ARM64 Inline Hook
// ============================================================
static void InstallBulletHitHook() {
    uintptr_t base = getAbsoluteAddress(libName, 0);
    if (!base) {
        LOGE("[HitSound] Failed to get base address of %s", libName);
        return;
    }

    uintptr_t target = base + OFFSET_BULLET_HIT_FUNC;
    uint32_t* p = reinterpret_cast<uint32_t*>(target);

    // Сохраняем первые 4 инструкции (16 байт)
    g_OrigInsn[0] = p[0];
    g_OrigInsn[1] = p[1];
    g_OrigInsn[2] = p[2];
    g_OrigInsn[3] = p[3];

    // Проверка: первая инструкция должна быть "sub sp, sp, #0xa0" (0xD10283FF)
    if (g_OrigInsn[0] != 0xD10283FF) {
        LOGW("[HitSound] Unexpected first instruction at target: 0x%08X", g_OrigInsn[0]);
        // Продолжаем — возможно, бинарник немного отличается
    }

    // Выделяем исполняемую память под trampoline + stub
    size_t stubSize = 256;
    g_Trampoline = mmap(nullptr, stubSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_Trampoline == MAP_FAILED) {
        LOGE("[HitSound] mmap failed");
        return;
    }

    uint32_t* s = reinterpret_cast<uint32_t*>(g_Trampoline);
    int i = 0;

    // --- asm stub ---
    // Сохраняем x29/x30 (чтобы blr не затёр lr оригинального вызова)
    s[i++] = 0xA9BF7BFD;  // stp x29, x30, [sp, #-16]!

    // Аргументы для C++ функции: x0 = victim (x19), x1 = attacker (x22)
    s[i++] = 0xAA1303E0;  // mov x0, x19
    s[i++] = 0xAA1603E1;  // mov x1, x22

    // blr &OnBulletHitNative
    s[i++] = 0x58000050;  // ldr x16, [pc, #8]
    s[i++] = 0xD63F0200;  // blr x16
    s[i++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnBulletHitNative));
    s[i++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnBulletHitNative) >> 32);

    // Восстанавливаем x29/x30
    s[i++] = 0xA8C17BFD;  // ldp x29, x30, [sp], #16

    // Оригинальные 4 инструкции (которые мы затёрли)
    s[i++] = g_OrigInsn[0];
    s[i++] = g_OrigInsn[1];
    s[i++] = g_OrigInsn[2];
    s[i++] = g_OrigInsn[3];

    // Прыжок обратно в оригинал: original_addr + 16
    s[i++] = 0x58000050;  // ldr x16, [pc, #8]
    s[i++] = 0xD61F0200;  // br x16
    s[i++] = static_cast<uint32_t>(target + 16);
    s[i++] = static_cast<uint32_t>((target + 16) >> 32);

    // --- патч оригинальной функции ---
    uintptr_t page = target & ~0xFFFULL;
    mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);

    // Пишем в начало функции: ldr x16, [pc, #8]; br x16; .quad stub_addr
    p[0] = 0x58000050;
    p[1] = 0xD61F0200;
    p[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_Trampoline));
    p[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_Trampoline) >> 32);

    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target) + 16);

    LOGI("[HitSound] Hook installed at 0x%llX -> trampoline 0x%llX", (unsigned long long)target,
         (unsigned long long)g_Trampoline);
}

// ============================================================
// ENTRY POINT
// ============================================================
void* main_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(libName));

    // Небольшая задержка, чтобы библиотека полностью инициализировалась
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
