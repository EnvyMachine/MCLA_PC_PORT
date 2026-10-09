// Controller input for the guest's XamInput* imports.
//
// Players 1-4 read the matching XInput controller. Player 1 also gets the
// keyboard while the game window has focus (merged with a controller if one is
// connected), so the game is playable without a pad:
//
//   W / S          right / left trigger (accelerate / brake)
//   A / D          left stick (steer)
//   arrow keys     D-pad (menus)
//   Enter          Start          Tab        Back
//   Space          A              Backspace  B
//   Left Shift     X              Left Ctrl  Y
//   Q / E          LB / RB        I J K L    right stick (camera)
//
// MCLA_INPUT_SCRIPT="40:start,46:a" presses buttons for 200 ms at the given
// seconds after start, for unattended test runs.
#pragma once

#include <windows.h>
#include <xinput.h>

#include <cstdint>

namespace input
{
    // State of the player's pad; false if the player has nothing connected.
    bool GetState(uint32_t user, XINPUT_STATE& state);

    // Whether the player has a controller (or the keyboard, for player 1).
    bool IsConnected(uint32_t user);

    void SetVibration(uint32_t user, uint16_t left, uint16_t right);
}
