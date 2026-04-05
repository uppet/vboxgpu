#pragma once
// Single-frame window capture using Windows.Graphics.Capture API (Win10 1903+).
// Captures hardware-accelerated content (Vulkan/DX).

#include <windows.h>
#include <cstdint>

// Capture the window content and save as BMP. Returns true on success.
bool captureWindowToBMP(HWND hwnd, const char* bmpPath);
