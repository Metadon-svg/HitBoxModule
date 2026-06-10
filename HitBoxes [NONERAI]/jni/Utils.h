#ifndef UTILS_H
#define UTILS_H

#include <cstdint>
#include <string>
#include <fstream>
#include <system_error>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <cstdlib>
#include <cstring>

#include "KittyInclude.hpp"

using DWORD = uint32_t;
using QWORD = uint64_t;

#if defined(__aarch64__)
    #define CACHE_FLUSH(start, end) __builtin___clear_cache((char*)(start), (char*)(end))
#else
    #define CACHE_FLUSH(start, end) do {} while(0)
#endif

using std::string;

uintptr_t findLibrary(const char* libraryName) {
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) return 0;

    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(libraryName) != std::string::npos) {
            size_t dashPos = line.find('-');
            if (dashPos != std::string::npos) {
                std::string addrStr = line.substr(0, dashPos);
                uintptr_t address = static_cast<uintptr_t>(strtoul(addrStr.c_str(), nullptr, 16));
                return address;
            }
        }
    }

    return 0;
}

uintptr_t getAbsoluteAddress(const char* libraryName, uintptr_t relativeAddr) {
    uintptr_t base = findLibrary(libraryName);
    return base ? base + relativeAddr : 0;
}

uintptr_t getAbsoluteAddressArch(const char* libraryName, uintptr_t relativeAddr) {
#if defined(__aarch64__)
    return getAbsoluteAddress(libraryName, relativeAddr);
#else
    return getAbsoluteAddress(libraryName, relativeAddr) + 1;
#endif
}

namespace Utils {
    template<typename T>
    bool WriteMemory(uintptr_t address, const T& value) {
        return KittyMemory::memWrite((void*) address, &value, sizeof(T));
    }

    template<typename T>
    T ReadMemory(uintptr_t address) {
        return *reinterpret_cast<volatile T*>(address);
    }
}

bool isLibraryLoaded(const char* libraryName) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) return false;

    string line;
    while (std::getline(maps, line)) {
        if (line.find(libraryName) != string::npos) {
            return true;
        }
    }
    return false;
}

uintptr_t string2Offset(const char* c) {
    char* end;
    if (sizeof(uintptr_t) == sizeof(unsigned long)) {
        return static_cast<uintptr_t>(strtoul(c, &end, 16));
    } else {
        return static_cast<uintptr_t>(strtoull(c, &end, 16));
    }
}

string string2Hex(const string& input) {
    static const char hex_digits[] = "0123456789ABCDEF";
    string output;
    output.reserve(input.length() * 2);
    
    for (unsigned char c : input) {
        output.push_back(hex_digits[c >> 4]);
        output.push_back(hex_digits[c & 0x0F]);
    }
    
    return output;
}

#endif // UTILS_H