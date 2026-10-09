// The game window. It lives on its own thread, which only pumps messages;
// rendering happens on the GPU command-processor thread.
#pragma once

#include <windows.h>

#include <cstdint>

namespace render
{
    // Creates the window (client area width x height) and returns once it exists.
    HWND CreateGameWindow(uint32_t width, uint32_t height);

    // Current client-area size (follows resizes).
    void GetClientSize(uint32_t& width, uint32_t& height);

    // The window, or nullptr before CreateGameWindow.
    HWND GameWindow();
}
