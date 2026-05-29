#pragma once
// Cross-platform dynamic-module helpers, so hook code can resolve symbols from the
// host (Cemu) process without sprinkling #ifdef _WIN32. Relies on the platform
// system headers pulled in by pch.h (Windows.h / dlfcn.h), which is force-included
// into every translation unit.

namespace platform {

#ifdef _WIN32
    using ModuleHandle = HMODULE;
#else
    using ModuleHandle = void*;
#endif

    // Handle to the current process's own module / global symbol table.
    inline ModuleHandle OpenSelfModule() {
#ifdef _WIN32
        return GetModuleHandleA(NULL);
#else
        return dlopen(NULL, RTLD_LAZY);
#endif
    }

    // Resolve an exported symbol by name; returns nullptr if not found.
    inline void* GetModuleSymbol(ModuleHandle module, const char* name) {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(module, name));
#else
        return dlsym(module, name);
#endif
    }

    inline void CloseModule(ModuleHandle module) {
        if (!module) return;
#ifdef _WIN32
        FreeLibrary(module);
#else
        dlclose(module);
#endif
    }

} // namespace platform
