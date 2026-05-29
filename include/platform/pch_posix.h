#pragma once
// Linux platform headers, libc shims, and macros for the precompiled header.
// Included from pch.h before the shared Vulkan/OpenXR includes.

#include <dlfcn.h>
#include <csignal>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <cpuid.h>
#include <strings.h>

// MSVC CRT names used by the shared codebase, mapped to their POSIX equivalents.
#define stricmp strcasecmp
#define strnicmp strncasecmp

// Backend selection consumed by the shared OpenXR includes in pch.h
#define XR_USE_GRAPHICS_API_VULKAN
