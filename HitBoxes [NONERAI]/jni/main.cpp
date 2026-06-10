--- START OF FILE main.cpp ---

#include <iostream>
#include <unistd.h>
#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>       // Добавлено для работы с dlopen/dlsym
#include <android/log.h> // Для логирования в Logcat

using namespace std;

#include "Utils.h"

// HIT BOXES (find string - PedModelInfo.cpp line: 85)
#if defined(__aarch64__)
    uintptr_t HEAD = 0x1422678;
    uintptr_t TORSO_1 = HEAD + 0x20;
    uintptr_t TORSO_2 = TORSO_1 + 0x20;
    uintptr_t MID = TORSO_2 + 0x20;
    uintptr_t LEFTARM = MID + 0x20;
    uintptr_t RIGHTARM = LEFTARM + 0x20;
    uintptr_t LEFTLEG_1 = RIGHTARM + 0x20;
    uintptr_t RIGHTLEG_1 = LEFTLEG_1 + 0x20;
    uintptr_t LEFTLEG_2 = RIGHTLEG_1 + 0x20;
    uintptr_t RIGHTLEG_2 = LEFTLEG_2 + 0x20;
#else
    uintptr_t HEAD = 0xD31C88;
    uintptr_t TORSO_1 = HEAD + 0x18;
    uintptr_t TORSO_2 = TORSO_1 + 0x18;
    uintptr_t MID = TORSO_2 + 0x18;
    uintptr_t LEFTARM = MID + 0x18;
    uintptr_t RIGHTARM = LEFTARM + 0x18;
    uintptr_t LEFTLEG_1 = RIGHTARM + 0x18;
    uintptr_t RIGHTLEG_1 = LEFTLEG_1 + 0x18;
    uintptr_t LEFTLEG_2 = RIGHTLEG_1 + 0x18;
    uintptr_t RIGHTLEG_2 = LEFTLEG_2 + 0x18;

    // Смещение командного процессора для ARM32, найденное на Шаге 11
    uintptr_t ProcessCommandOffset = 0x39300c;
#endif

#define libName "libblackrussia-client.so"
#define LOG_TAG "BRClientMod"

// Функция динамической перезаписи хитбоксов в памяти
void applyHitboxScale(float MultiplyValue) {
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, HEAD), 0.15f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, TORSO_1), 0.2f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, TORSO_2), 0.25f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, MID), 0.25f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, LEFTARM), 0.16f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, RIGHTARM), 0.16f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, LEFTLEG_1), 0.2f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, RIGHTLEG_1), 0.2f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, LEFTLEG_2), 0.15f * MultiplyValue);
    Utils::WriteMemory<float>(getAbsoluteAddress(libName, RIGHTLEG_2), 0.15f * MultiplyValue);

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Hitbox multiplier set to: %f", MultiplyValue);
}

// Указатель на функцию хука (будет инициализирован динамически)
void (*MSHookFunction)(void *symbol, void *replace, void **result) = nullptr;

// Указатель на оригинальную функцию
int (*ProcessCommand_original)(const char* cmdText) = nullptr;

// Наша функция-перехватчик команд
int ProcessCommand_hook(const char* cmdText) {
    if (cmdText && strncmp(cmdText, "/sizehb ", 8) == 0) {
        // Парсим множитель из введенной команды (например, "/sizehb 2" -> 2.0f)
        float scale = atof(cmdText + 8);
        if (scale > 0.0f) {
            applyHitboxScale(scale);
        }
        return 1; // Команда обработана локально, не отправляем её на сервер
    }
    return ProcessCommand_original(cmdText);
}

void *main_thread(void *) {
    do { sleep(1); } while (!isLibraryLoaded(libName));

    // При старте игры устанавливаем стандартное умножение на 3
    applyHitboxScale(3.0f);

#if !defined(__aarch64__)
    // Динамически разрешаем адрес MSHookFunction в адресном пространстве процесса
    MSHookFunction = (void (*)(void*, void*, void**))dlsym(RTLD_DEFAULT, "MSHookFunction");

    if (MSHookFunction) {
        // Для ARM32 устанавливаем хук на обработчик команд
        void* process_cmd_addr = (void*)getAbsoluteAddress(libName, ProcessCommandOffset);
        if (process_cmd_addr) {
            MSHookFunction(process_cmd_addr, (void*)&ProcessCommand_hook, (void**)&ProcessCommand_original);
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "MSHookFunction successfully resolved and hook applied.");
        }
    } else {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Failed to resolve MSHookFunction! Hook NOT applied.");
    }
#endif

    pthread_exit(nullptr);
    return nullptr;
}

__attribute__((constructor)) void _init(){
    pthread_t ptid;
    pthread_create(&ptid, NULL, main_thread, NULL);
}
