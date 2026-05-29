#include "pch.h"

#include "logger.h"

#ifdef _WIN32
HANDLE Log::consoleHandle = NULL;
double Log::timeFrequency = 0.0f;
#endif
std::ofstream Log::logFile;
std::mutex Log::logMutex;

static void LogSystemHardwareInfo() {
#ifdef _WIN32
    int cpuInfo[4] = {0, 0, 0, 0};
    __cpuid(cpuInfo, 0x80000000);
    const unsigned int maxExId = static_cast<unsigned int>(cpuInfo[0]);

    std::string cpuBrand;
    if (maxExId >= 0x80000004) {
        char brand[49] = {};
        __cpuid(reinterpret_cast<int*>(brand + 0), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        cpuBrand = brand;
        while (!cpuBrand.empty() && cpuBrand.front() == ' ') cpuBrand.erase(cpuBrand.begin());
        while (!cpuBrand.empty() && cpuBrand.back() == ' ') cpuBrand.pop_back();
        if (!cpuBrand.empty()) {
            Log::print<INFO>("CPU: {}", cpuBrand.c_str());
        }
    }

    MEMORYSTATUSEX statex{};
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex)) {
        const double totalGiB = double(statex.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        Log::print<INFO>("RAM: {:.2f} GiB", totalGiB);
    }
#else
    // CPU brand via cpuid.h
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) && eax >= 0x80000004) {
        char brand[49] = {};
        __get_cpuid(0x80000002, reinterpret_cast<unsigned int*>(brand + 0), reinterpret_cast<unsigned int*>(brand + 4), reinterpret_cast<unsigned int*>(brand + 8), reinterpret_cast<unsigned int*>(brand + 12));
        __get_cpuid(0x80000003, reinterpret_cast<unsigned int*>(brand + 16), reinterpret_cast<unsigned int*>(brand + 20), reinterpret_cast<unsigned int*>(brand + 24), reinterpret_cast<unsigned int*>(brand + 28));
        __get_cpuid(0x80000004, reinterpret_cast<unsigned int*>(brand + 32), reinterpret_cast<unsigned int*>(brand + 36), reinterpret_cast<unsigned int*>(brand + 40), reinterpret_cast<unsigned int*>(brand + 44));
        std::string cpuBrand(brand);
        while (!cpuBrand.empty() && cpuBrand.front() == ' ') cpuBrand.erase(cpuBrand.begin());
        while (!cpuBrand.empty() && cpuBrand.back() == ' ') cpuBrand.pop_back();
        if (!cpuBrand.empty()) {
            Log::print<INFO>("CPU: {}", cpuBrand.c_str());
        }
    }

    struct sysinfo si{};
    if (sysinfo(&si) == 0) {
        const double totalGiB = double(si.totalram) * si.mem_unit / (1024.0 * 1024.0 * 1024.0);
        Log::print<INFO>("RAM: {:.2f} GiB", totalGiB);
    }
#endif
}

Log::Log() {
#ifdef _WIN32
    AllocConsole();
    SetConsoleTitleA("BetterVR Debugging Console");
    consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    LARGE_INTEGER timeLI;
    QueryPerformanceFrequency(&timeLI);
    timeFrequency = double(timeLI.QuadPart) / 1000.0;
#endif
#ifdef _WIN32
    logFile.open("BetterVR_log.txt", std::ios::out | std::ios::app);
#else
    logFile.open("BetterVR_log.txt", std::ios::out | std::ios::trunc);
#endif
    Log::print<INFO>("Successfully started BetterVR!");
    LogSystemHardwareInfo();
}

Log::~Log() {
    Log::print<INFO>("Shutting down BetterVR...");
#ifdef _WIN32
    FreeConsole();
#endif
    if (logFile.is_open()) {
        logFile.close();
    }
}

#ifdef _WIN32
void Log::printTimeElapsed(const char* message_prefix, LARGE_INTEGER time) {
    LARGE_INTEGER timeNow;
    QueryPerformanceCounter(&timeNow);
    Log::print<INFO>("{}: {} ms", message_prefix, double(time.QuadPart - timeNow.QuadPart) / timeFrequency);
}
#else
void Log::printTimeElapsed(const char* message_prefix, std::chrono::high_resolution_clock::time_point time) {
    auto now = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - time).count();
    Log::print<INFO>("{}: {} ms", message_prefix, ms);
}
#endif
