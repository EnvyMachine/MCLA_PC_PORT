#include "input.h"
#include "log.h"
#include "../render/window.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace input
{
    namespace
    {
        std::mutex s_lock;
        // XInputGetState on an empty port can take milliseconds, so empty ports
        // are probed once a second.
        bool s_connected[XUSER_MAX_COUNT];
        uint64_t s_nextProbe[XUSER_MAX_COUNT];
        XINPUT_GAMEPAD s_last[XUSER_MAX_COUNT];
        DWORD s_packet[XUSER_MAX_COUNT];

        struct ScriptedPress
        {
            uint64_t atMs;
            uint16_t buttons;
            uint64_t durationMs = 200;
            char analog = 0; // 'R'/'L' trigger, '<'/'>' left stick
        };

        // Pseudo-buttons for analog inputs held by MCLA_INPUT_SCRIPT.
        char AnalogFromName(const std::string& name)
        {
            if (name == "rt") return 'R';
            if (name == "lt") return 'L';
            if (name == "sl") return '<';
            if (name == "sr") return '>';
            if (name == "ru") return '^';
            if (name == "rd") return 'v';
            return 0;
        }
        const uint64_t s_startMs = GetTickCount64();

        uint16_t ButtonFromName(const std::string& name)
        {
            static const struct
            {
                const char* name;
                uint16_t bit;
            } kButtons[] = {
                { "up", XINPUT_GAMEPAD_DPAD_UP }, { "down", XINPUT_GAMEPAD_DPAD_DOWN },
                { "left", XINPUT_GAMEPAD_DPAD_LEFT }, { "right", XINPUT_GAMEPAD_DPAD_RIGHT },
                { "start", XINPUT_GAMEPAD_START }, { "back", XINPUT_GAMEPAD_BACK },
                { "ls", XINPUT_GAMEPAD_LEFT_THUMB }, { "rs", XINPUT_GAMEPAD_RIGHT_THUMB },
                { "lb", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "rb", XINPUT_GAMEPAD_RIGHT_SHOULDER },
                { "a", XINPUT_GAMEPAD_A }, { "b", XINPUT_GAMEPAD_B }, { "x", XINPUT_GAMEPAD_X }, { "y", XINPUT_GAMEPAD_Y },
            };
            for (const auto& b : kButtons)
                if (name == b.name)
                    return b.bit;
            return 0;
        }

        // MCLA_INPUT_SCRIPT="40:start,46.5:a,60:rt:5" - <seconds>:<button>[:<hold seconds>].
        // Buttons as in kButtons, plus rt/lt (triggers), sl/sr (left stick left/right)
        // and ru/rd (right stick up/down).
        const std::vector<ScriptedPress>& Script()
        {
            static const std::vector<ScriptedPress> script = [] {
                std::vector<ScriptedPress> presses;
                char buffer[2048];
                DWORD length = GetEnvironmentVariableA("MCLA_INPUT_SCRIPT", buffer, sizeof(buffer));
                if (!length || length >= sizeof(buffer))
                    return presses;
                std::string text(buffer, length);
                size_t pos = 0;
                while (pos < text.size())
                {
                    size_t end = text.find(',', pos);
                    std::string item = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    size_t colon = item.find(':');
                    if (colon != std::string::npos)
                    {
                        std::string name = item.substr(colon + 1);
                        uint64_t durationMs = 200;
                        if (size_t hold = name.find(':'); hold != std::string::npos)
                        {
                            durationMs = uint64_t(atof(name.substr(hold + 1).c_str()) * 1000.0);
                            name.resize(hold);
                        }
                        uint16_t bit = ButtonFromName(name);
                        char analog = AnalogFromName(name);
                        if (bit || analog)
                            presses.push_back({ uint64_t(atof(item.substr(0, colon).c_str()) * 1000.0), bit, durationMs, analog });
                        else
                            LOGI("input: unknown button in MCLA_INPUT_SCRIPT: %s", item.c_str());
                    }
                    if (end == std::string::npos)
                        break;
                    pos = end + 1;
                }
                if (!presses.empty())
                    LOGI("input: %zu scripted button presses", presses.size());
                return presses;
            }();
            return script;
        }

        bool Down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

        int16_t Axis(int negative, int positive)
        {
            return int16_t((Down(positive) ? 32767 : 0) - (Down(negative) ? 32767 : 0));
        }

        XINPUT_GAMEPAD Keyboard()
        {
            XINPUT_GAMEPAD k{};
            HWND window = render::GameWindow();
            if (window && GetForegroundWindow() == window)
            {
                static const struct
                {
                    int key;
                    uint16_t bit;
                } kKeys[] = {
                    { VK_UP, XINPUT_GAMEPAD_DPAD_UP }, { VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
                    { VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
                    { VK_RETURN, XINPUT_GAMEPAD_START }, { VK_TAB, XINPUT_GAMEPAD_BACK },
                    { 'Z', XINPUT_GAMEPAD_LEFT_THUMB }, { 'C', XINPUT_GAMEPAD_RIGHT_THUMB },
                    { 'Q', XINPUT_GAMEPAD_LEFT_SHOULDER }, { 'E', XINPUT_GAMEPAD_RIGHT_SHOULDER },
                    { VK_SPACE, XINPUT_GAMEPAD_A }, { VK_BACK, XINPUT_GAMEPAD_B },
                    { VK_LSHIFT, XINPUT_GAMEPAD_X }, { VK_LCONTROL, XINPUT_GAMEPAD_Y },
                };
                for (const auto& key : kKeys)
                    if (Down(key.key) && !(key.key == VK_RETURN && Down(VK_MENU))) // Alt+Enter: fullscreen
                        k.wButtons |= key.bit;
                k.bRightTrigger = Down('W') ? 255 : 0;
                k.bLeftTrigger = Down('S') ? 255 : 0;
                k.sThumbLX = Axis('A', 'D');
                k.sThumbRX = Axis('J', 'L');
                k.sThumbRY = Axis('K', 'I');
            }
            uint64_t now = GetTickCount64() - s_startMs;
            for (const ScriptedPress& press : Script())
            {
                if (now < press.atMs || now >= press.atMs + press.durationMs)
                    continue;
                k.wButtons |= press.buttons;
                switch (press.analog)
                {
                case 'R': k.bRightTrigger = 255; break;
                case 'L': k.bLeftTrigger = 255; break;
                case '<': k.sThumbLX = -32767; break;
                case '>': k.sThumbLX = 32767; break;
                case '^': k.sThumbRY = 32767; break;
                case 'v': k.sThumbRY = -32767; break;
                }
            }
            return k;
        }

        // Caller holds s_lock.
        bool Poll(uint32_t user, XINPUT_STATE& state)
        {
            uint64_t now = GetTickCount64();
            if (!s_connected[user] && now < s_nextProbe[user])
                return false;
            bool connected = XInputGetState(user, &state) == ERROR_SUCCESS;
            if (connected != s_connected[user])
                LOGI("input: controller %u %s", user + 1, connected ? "connected" : "disconnected");
            s_connected[user] = connected;
            if (!connected)
                s_nextProbe[user] = now + 1000;
            return connected;
        }
    }

    bool GetState(uint32_t user, XINPUT_STATE& state)
    {
        if (user >= XUSER_MAX_COUNT)
            return false;
        std::lock_guard guard(s_lock);
        XINPUT_STATE pad{};
        bool connected = Poll(user, pad);
        if (user == 0)
        {
            XINPUT_GAMEPAD k = Keyboard();
            XINPUT_GAMEPAD& g = pad.Gamepad;
            g.wButtons |= k.wButtons;
            g.bLeftTrigger = std::max(g.bLeftTrigger, k.bLeftTrigger);
            g.bRightTrigger = std::max(g.bRightTrigger, k.bRightTrigger);
            if (k.sThumbLX || k.sThumbLY)
            {
                g.sThumbLX = k.sThumbLX;
                g.sThumbLY = k.sThumbLY;
            }
            if (k.sThumbRX || k.sThumbRY)
            {
                g.sThumbRX = k.sThumbRX;
                g.sThumbRY = k.sThumbRY;
            }
            connected = true;
        }
        if (!connected)
            return false;
        if (memcmp(&pad.Gamepad, &s_last[user], sizeof(XINPUT_GAMEPAD)) != 0)
        {
            s_last[user] = pad.Gamepad;
            s_packet[user]++;
        }
        state = pad;
        state.dwPacketNumber = s_packet[user];
        return true;
    }

    bool IsConnected(uint32_t user)
    {
        if (user >= XUSER_MAX_COUNT)
            return false;
        if (user == 0)
            return true;
        std::lock_guard guard(s_lock);
        XINPUT_STATE state;
        return Poll(user, state);
    }

    void SetVibration(uint32_t user, uint16_t left, uint16_t right)
    {
        if (user >= XUSER_MAX_COUNT || !s_connected[user])
            return;
        XINPUT_VIBRATION v{ left, right };
        XInputSetState(user, &v);
    }
}
