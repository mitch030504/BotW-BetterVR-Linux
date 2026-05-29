#include "pch.h"
#include "layer_active.h"

#include <cstring>
#include <cstdlib>
#include <string_view>

namespace VRLayer {

bool IsLayerActiveForProcess() {
    static const bool active = [] {
        if (const char* disabled = std::getenv("DISABLE_BETTERVR_MOD")) {
            if (std::strcmp(disabled, "1") == 0) return false;
        }

#ifdef _WIN32
        HMODULE module = GetModuleHandleA(nullptr);
        if (module &&
            GetProcAddress(module, "gameMeta_getTitleId") &&
            GetProcAddress(module, "memory_getBase") &&
            GetProcAddress(module, "osLib_registerHLEFunction")) {
            return true;
        }
        char exePath[MAX_PATH] = {};
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len > 0) {
            std::string_view path(exePath, len);
            return path.find("Cemu") != std::string_view::npos ||
                   path.find("cemu") != std::string_view::npos;
        }
        return false;
#else
        void* self = dlopen(nullptr, RTLD_LAZY);
        if (self &&
            dlsym(self, "gameMeta_getTitleId") &&
            dlsym(self, "memory_getBase") &&
            dlsym(self, "osLib_registerHLEFunction")) {
            return true;
        }

        char exePath[4096] = {};
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            std::string_view path(exePath, (size_t)len);
            return path.find("Cemu") != std::string_view::npos ||
                   path.find("cemu") != std::string_view::npos;
        }
        return false;
#endif
    }();
    return active;
}

}
