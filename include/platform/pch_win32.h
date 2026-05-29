#pragma once
// Windows / D3D12 platform headers, macros, and helpers for the precompiled header.
// Included from pch.h (before the shared Vulkan/OpenXR includes) so the backend-
// selection macros are visible to them. Windows.h's include guard means ERROR /
// CreateEvent / CreateSemaphore are defined once and undef'd once below, even
// though later headers (d3d12, openxr) re-include <Windows.h>.

#include <Windows.h>
#include <winrt/base.h>
#include <shellapi.h>

// These macros mess with some of Vulkan's functions
#undef ERROR
#undef CreateEvent
#undef CreateSemaphore

// Backend selection consumed by the shared Vulkan/OpenXR includes in pch.h
#define VK_USE_PLATFORM_WIN32_KHR
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12

// D3D12 includes (must precede openxr_platform.h, which references D3D12 types
// when XR_USE_GRAPHICS_API_D3D12 is defined)
#include <d3d12.h>
#include <D3Dcompiler.h>
#include <dxgi1_6.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "dxguid.lib")

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

inline std::string wcharToUtf8(const wchar_t* wstr) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size_needed, nullptr, nullptr);
    return str;
}
