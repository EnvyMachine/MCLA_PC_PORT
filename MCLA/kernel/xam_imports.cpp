// XAM imports: user/profile, content (saves), UI, notifications, tasks, loader
// and controller input. Everything Xbox Live related reports "offline".
#include "imports.h"
#include "../runtime/input.h"
#include "../runtime/vfs.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <vector>

namespace
{
    constexpr uint64_t kOfflineXuid = 0xE000000000C0FFEEull;
    const char kGamertag[] = "Player";

    // XOVERLAPPED: be32 result, be32 length, be32 context, be32 event,
    //              be32 completion_routine, be32 completion_context, be32 extended_error
    uint32_t Complete(uint32_t overlapped, uint32_t result, uint32_t length = 0)
    {
        if (!overlapped)
            return result;
        CompleteOverlapped(overlapped, result, length);
        return xerror::IoPending;
    }

    std::wstring ReadWide(uint32_t addr)
    {
        std::wstring s;
        if (addr)
            for (uint16_t c; (c = Load16(addr)) != 0; addr += 2)
                s += wchar_t(c);
        return s;
    }

    std::string Narrow(const std::wstring& w)
    {
        std::string s;
        for (wchar_t c : w)
            s += c < 0x80 ? char(c) : '?';
        return s;
    }

    // ---- enumerators (content, achievements, stats) ----------------------------

    struct Enumerator : kern::HostObject
    {
        uint32_t itemSize = 0;
        std::deque<std::vector<uint8_t>> items; // guest-format items
    };

    std::filesystem::path ContentDir(uint32_t contentType)
    {
        char sub[32];
        snprintf(sub, sizeof(sub), "%08X\\%08X", g_xex.titleId, contentType);
        return vfs::UserDir() / "content" / sub;
    }

    // XCONTENT_DATA: be32 device_id, be32 content_type, be16 display_name[128], char file_name[42]
    constexpr uint32_t kContentDataSize = 0x134;
}

void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
{
    Store32(overlapped + 0x00, result);
    Store32(overlapped + 0x04, length);
    // The extended error is the HRESULT form; titles test it (MCLA's save code
    // accepts an empty save enumeration only as 0x80070012, "no more files").
    Store32(overlapped + 0x18, result ? 0x80070000u | (result & 0xFFFF) : 0);
    if (uint32_t ev = Load32(overlapped + 0x0C))
        if (uint32_t obj = kern::ObjectFromHandle(ev))
            kern::SetEvent(obj);
    if (uint32_t routine = Load32(overlapped + 0x10))
        LOGI("XOVERLAPPED completion routine %08X not invoked", routine);
}

// ---- system / loader ----------------------------------------------------------

GUEST_IMPORT(XamGetSystemVersion) { TRACE("XamGetSystemVersion"); ctx.r3.u64 = 0; }

GUEST_IMPORT(XamGetExecutionId)
{
    TRACE("XamGetExecutionId");
    Store32(ctx.r3.u32, XexHeaderField(g_xex.header, 0x00040006));
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(XNetLogonGetTitleID) { TRACE("XNetLogonGetTitleID"); ctx.r3.u64 = g_xex.titleId; }

GUEST_IMPORT(XamLoaderGetLaunchDataSize)
{
    TRACE("XamLoaderGetLaunchDataSize");
    if (ctx.r3.u32)
        Store32(ctx.r3.u32, 0);
    ctx.r3.u64 = xerror::NotFound;
}

GUEST_IMPORT(XamLoaderGetLaunchData) { TRACE("XamLoaderGetLaunchData"); ctx.r3.u64 = xerror::NotFound; }
GUEST_IMPORT(XamLoaderSetLaunchData) { TRACE("XamLoaderSetLaunchData"); ctx.r3.u64 = 0; }

GUEST_IMPORT(XamLoaderLaunchTitle)
{
    LOGI("XamLoaderLaunchTitle('%s'): title wants to relaunch; exiting", ctx.r3.u32 ? GuestPtr<const char>(ctx.r3.u32) : "");
    LogFlush();
    ExitProcess(0);
}

GUEST_IMPORT(XamLoaderTerminateTitle)
{
    LOGI("XamLoaderTerminateTitle: exiting");
    LogFlush();
    ExitProcess(0);
}

// ---- users / profile ----------------------------------------------------------

GUEST_IMPORT(XamUserGetSigninState)
{
    TRACE("XamUserGetSigninState");
    ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0; // user 0 signed in locally
}

GUEST_IMPORT(XamUserGetXUID)
{
    TRACE("XamUserGetXUID");
    if (ctx.r3.u32 != 0)
    {
        ctx.r3.u64 = xerror::NoSuchUser;
        return;
    }
    if (ctx.r5.u32)
        Store64(ctx.r5.u32, kOfflineXuid);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetName)
{
    TRACE("XamUserGetName");
    if (ctx.r3.u32 != 0)
    {
        ctx.r3.u64 = xerror::NoSuchUser;
        return;
    }
    uint32_t size = ctx.r5.u32;
    if (size)
    {
        uint32_t n = std::min<uint32_t>(size - 1, sizeof(kGamertag) - 1);
        memcpy(g_base + ctx.r4.u32, kGamertag, n);
        Store8(ctx.r4.u32 + n, 0);
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetSigninInfo)
{
    TRACE("XamUserGetSigninInfo");
    uint32_t info = ctx.r5.u32;
    memset(g_base + info, 0, 0x28);
    if (ctx.r3.u32 != 0)
    {
        ctx.r3.u64 = xerror::NoSuchUser;
        return;
    }
    Store64(info + 0x00, kOfflineXuid);
    Store32(info + 0x0C, 1); // signed in locally
    memcpy(g_base + info + 0x18, kGamertag, sizeof(kGamertag));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserCheckPrivilege)
{
    TRACE("XamUserCheckPrivilege");
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, 0); // no online privileges
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserAreUsersFriends)
{
    TRACE("XamUserAreUsersFriends");
    // (user, xuids, count, result*, overlapped)
    if (ctx.r6.u32)
        Store32(ctx.r6.u32, 0);
    ctx.r3.u64 = Complete(ctx.r7.u32, 0);
}

GUEST_IMPORT(XamUserReadProfileSettings)
{
    TRACE("XamUserReadProfileSettings");
    // (title_id, user, xuid_count, xuids, setting_count, setting_ids, buffer_size*, buffer, overlapped)
    uint32_t settingCount = ctx.r7.u32, settingIds = ctx.r8.u32, pBufferSize = ctx.r9.u32, buffer = ctx.r10.u32;
    uint32_t overlapped = Arg(ctx, 8);
    uint32_t xuidCount = std::max(ctx.r5.u32, 1u);
    uint32_t needed = 8 + settingCount * xuidCount * 0x28;
    if (!buffer || Load32(pBufferSize) < needed)
    {
        Store32(pBufferSize, needed);
        ctx.r3.u64 = xerror::InsufficientBuffer;
        return;
    }
    // X_USER_READ_PROFILE_SETTING_RESULT { be32 count; be32 settings_ptr; } followed
    // by X_USER_PROFILE_SETTING { be32 source; pad; be64 user; be32 id; pad; X_USER_DATA (16) }.
    memset(g_base + buffer, 0, needed);
    {
        std::string ids;
        char text[16];
        for (uint32_t i = 0; i < settingCount && i < 24; i++)
        {
            snprintf(text, sizeof(text), " %08X", Load32(settingIds + i * 4));
            ids += text;
        }
        LOGI("XamUserReadProfileSettings: user %u, %u settings:%s", ctx.r4.u32, settingCount, ids.c_str());
    }
    Store32(buffer + 0, settingCount * xuidCount);
    Store32(buffer + 4, buffer + 8);
    // Dashboard defaults for the gamer settings (values from Xenia's user profile).
    // MCLA reads the racing settings: without values its control mapping puts the
    // accelerator on the right stick instead of the trigger.
    auto defaultValue = [](uint32_t id, bool& known) -> uint32_t {
        known = true;
        switch (id)
        {
        case 0x10040002: return 0;           // XPROFILE_GAMER_YAXIS_INVERSION
        case 0x10040003: return 3;           // XPROFILE_OPTION_CONTROLLER_VIBRATION (on)
        case 0x10040015: return 0;           // XPROFILE_GAMER_DIFFICULTY (normal)
        case 0x10040018: return 0;           // XPROFILE_GAMER_CONTROL_SENSITIVITY (medium)
        case 0x10040022: return 1;           // XPROFILE_GAMER_ACTION_AUTO_AIM
        case 0x10040023: return 0;           // XPROFILE_GAMER_ACTION_AUTO_CENTER
        case 0x10040024: return 1;           // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL (left stick)
        case 0x10040026: return 0;           // XPROFILE_GAMER_RACE_TRANSMISSION (automatic)
        case 0x10040027: return 0;           // XPROFILE_GAMER_RACE_CAMERA_LOCATION (behind)
        case 0x10040028: return 0;           // XPROFILE_GAMER_RACE_BRAKE_CONTROL (trigger)
        case 0x10040029: return 0;           // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL (trigger)
        default: known = false; return 0;
        }
    };
    for (uint32_t u = 0; u < xuidCount; u++)
    {
        for (uint32_t i = 0; i < settingCount; i++)
        {
            uint32_t id = Load32(settingIds + i * 4);
            uint32_t s = buffer + 8 + (u * settingCount + i) * 0x28;
            bool known;
            uint32_t value = defaultValue(id, known);
            Store32(s + 0x00, known ? 1 : 0); // XSOURCE_DEFAULT, or XSOURCE_NO_VALUE (title defaults)
            Store32(s + 0x08, ctx.r4.u32);
            Store32(s + 0x10, id);
            Store8(s + 0x18, uint8_t((id >> 28) & 0xF)); // data type encoded in the id
            if (known)
                Store32(s + 0x20, value);
        }
    }
    ctx.r3.u64 = Complete(overlapped, 0);
}

GUEST_IMPORT(XamUserCreateAchievementEnumerator)
{
    TRACE("XamUserCreateAchievementEnumerator");
    // (title_id, user, xuid, flags, start, count, buffer_size*, handle*)
    auto e = std::make_shared<Enumerator>();
    e->itemSize = 0x34;
    Store32(Arg(ctx, 6), 0x34 * std::max(Arg(ctx, 5), 1u));
    Store32(Arg(ctx, 7), kern::CreateHandle(kern::HandleKind::Enumerator, kern::NewEvent(true, true), e));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserCreateStatsEnumerator)
{
    TRACE("XamUserCreateStatsEnumerator");
    LOGI("XamUserCreateStatsEnumerator: leaderboards unavailable offline");
    ctx.r3.u64 = xerror::FunctionFailed;
}

// ---- UI -------------------------------------------------------------------------

GUEST_IMPORT(XamShowSigninUI) { TRACE("XamShowSigninUI"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamShowAchievementsUI) { TRACE("XamShowAchievementsUI"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamShowFriendsUI) { TRACE("XamShowFriendsUI"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamShowGamerCardUIForXUID) { TRACE("XamShowGamerCardUIForXUID"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamShowMarketplaceUI) { TRACE("XamShowMarketplaceUI"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamShowPlayerReviewUI) { TRACE("XamShowPlayerReviewUI"); ctx.r3.u64 = 0; }

GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    TRACE("XamShowDeviceSelectorUI");
    // (user, content_type, content_flags, total_requested (64-bit), device_id*, overlapped)
    Store32(ctx.r7.u32, 1); // the hard drive
    ctx.r3.u64 = Complete(ctx.r8.u32, 0);
}

GUEST_IMPORT(XamShowMessageBoxUIEx)
{
    TRACE("XamShowMessageBoxUIEx");
    // (user, title, text, button_count, buttons, focus, flags, result*, overlapped)
    LOGI("message box: [%s] %s", Narrow(ReadWide(ctx.r4.u32)).c_str(), Narrow(ReadWide(ctx.r5.u32)).c_str());
    if (ctx.r10.u32)
        Store32(ctx.r10.u32, 0); // first button
    ctx.r3.u64 = Complete(Arg(ctx, 8), 0);
}

GUEST_IMPORT(XamShowDirtyDiscErrorUI)
{
    LOGI("FATAL: XamShowDirtyDiscErrorUI: the title failed to read its data");
    LogFlush();
    ExitProcess(1);
}

// ---- content (saves) ------------------------------------------------------------

GUEST_IMPORT(XamContentGetDeviceState)
{
    TRACE("XamContentGetDeviceState");
    ctx.r3.u64 = Complete(ctx.r4.u32, ctx.r3.u32 == 1 ? 0 : xerror::DeviceNotConnected);
}

GUEST_IMPORT(XamContentGetDeviceData)
{
    TRACE("XamContentGetDeviceData");
    if (ctx.r3.u32 != 1)
    {
        ctx.r3.u64 = xerror::DeviceNotConnected;
        return;
    }
    uint32_t d = ctx.r4.u32;
    memset(g_base + d, 0, 0x50);
    Store32(d + 0x00, 1);
    Store32(d + 0x04, 1); // XCONTENTDEVICETYPE_HDD
    Store64(d + 0x08, 20ull << 30);
    Store64(d + 0x10, 10ull << 30);
    const char name[] = "Hard Drive";
    for (uint32_t i = 0; name[i]; i++)
        Store16(d + 0x18 + i * 2, uint16_t(name[i]));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamContentCreateEx)
{
    TRACE("XamContentCreateEx");
    // (user, root_name, content_data, flags, disposition*, license_mask*, cache_size, content_size, overlapped)
    std::string root = GuestPtr<const char>(ctx.r4.u32);
    uint32_t data = ctx.r5.u32, flags = ctx.r6.u32, pDisposition = ctx.r7.u32, pLicense = ctx.r8.u32;
    uint32_t overlapped = Arg(ctx, 8);
    uint32_t contentType = Load32(data + 4);
    std::string fileName(GuestPtr<const char>(data + 0x108), strnlen(GuestPtr<const char>(data + 0x108), 42));
    std::filesystem::path dir = ContentDir(contentType) / fileName;
    bool exists = std::filesystem::exists(dir);

    uint32_t result = 0;
    switch (flags & 0xF)
    {
    case 1: // CREATE_NEW
        result = exists ? xerror::AlreadyExists : 0;
        break;
    case 3: // OPEN_EXISTING
    case 5: // TRUNCATE_EXISTING
        result = exists ? 0 : xerror::PathNotFound;
        break;
    default: // CREATE_ALWAYS, OPEN_ALWAYS
        break;
    }
    if (result == 0)
    {
        if ((flags & 0xF) == 2 || (flags & 0xF) == 5)
            std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        vfs::Mount(root, dir);
        if (pDisposition)
            Store32(pDisposition, exists ? 2 : 1);
        if (pLicense)
            Store32(pLicense, 0xFFFFFFFF);
    }
    LOGI("XamContentCreateEx('%s', type %X, '%s', flags %X) -> %X", root.c_str(), contentType, fileName.c_str(), flags, result);
    ctx.r3.u64 = Complete(overlapped, result);
}

GUEST_IMPORT(XamContentClose)
{
    TRACE("XamContentClose");
    vfs::Unmount(GuestPtr<const char>(ctx.r3.u32));
    ctx.r3.u64 = Complete(ctx.r4.u32, 0);
}

GUEST_IMPORT(XamContentCreateEnumerator)
{
    TRACE("XamContentCreateEnumerator");
    // (user, device_id, content_type, flags, items_per_enumerate, buffer_size*, handle*)
    uint32_t contentType = ctx.r5.u32, itemsPer = std::max(ctx.r7.u32, 1u);
    auto e = std::make_shared<Enumerator>();
    e->itemSize = kContentDataSize;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(ContentDir(contentType), ec))
    {
        if (!entry.is_directory())
            continue;
        std::vector<uint8_t> item(kContentDataSize, 0);
        auto be32 = [&](size_t off, uint32_t v) { uint32_t b = __builtin_bswap32(v); memcpy(&item[off], &b, 4); };
        be32(0, 1);
        be32(4, contentType);
        std::string name = entry.path().filename().string();
        for (size_t i = 0; i < name.size() && i < 127; i++)
        {
            uint16_t c = __builtin_bswap16(uint16_t(name[i]));
            memcpy(&item[8 + i * 2], &c, 2);
        }
        memcpy(&item[0x108], name.data(), std::min<size_t>(name.size(), 41));
        e->items.push_back(std::move(item));
    }
    if (ctx.r8.u32)
        Store32(ctx.r8.u32, itemsPer * kContentDataSize);
    Store32(ctx.r9.u32, kern::CreateHandle(kern::HandleKind::Enumerator, kern::NewEvent(true, true), e));
    LOGI("XamContentCreateEnumerator(type %X): %zu items", contentType, e->items.size());
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamEnumerate)
{
    TRACE("XamEnumerate");
    // (handle, flags, buffer, buffer_size, items_returned*, overlapped)
    kern::HandleEntry h;
    if (!kern::LookupHandle(ctx.r3.u32, h) || h.kind != kern::HandleKind::Enumerator)
    {
        ctx.r3.u64 = xerror::InvalidHandle;
        return;
    }
    auto e = std::static_pointer_cast<Enumerator>(h.host);
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32, count = 0;
    while (!e->items.empty() && (count + 1) * e->itemSize <= size)
    {
        memcpy(g_base + buffer + count * e->itemSize, e->items.front().data(), e->itemSize);
        e->items.pop_front();
        count++;
    }
    if (ctx.r7.u32)
        Store32(ctx.r7.u32, count);
    ctx.r3.u64 = Complete(ctx.r8.u32, count ? 0 : xerror::NoMoreFiles, count);
}

GUEST_IMPORT(XamCreateEnumeratorHandle)
{
    LOGI("XamCreateEnumeratorHandle: unsupported");
    ctx.r3.u64 = xerror::FunctionFailed;
}

GUEST_IMPORT(XamGetPrivateEnumStructureFromHandle)
{
    LOGI("XamGetPrivateEnumStructureFromHandle: unsupported");
    ctx.r3.u64 = xerror::InvalidHandle;
}

// ---- notifications, tasks, messaging -------------------------------------------

GUEST_IMPORT(XamNotifyCreateListener)
{
    TRACE("XamNotifyCreateListener");
    ctx.r3.u64 = kern::CreateHandle(kern::HandleKind::Notification, kern::NewEvent(true, false));
}

GUEST_IMPORT(XNotifyGetNext)
{
    TRACE("XNotifyGetNext");
    ctx.r3.u64 = 0; // nothing pending
}

GUEST_IMPORT(XamTaskSchedule)
{
    TRACE("XamTaskSchedule");
    // (callback, context, unknown, handle*)
    GuestThread* t = thread::Create(0, ctx.r3.u32, ctx.r4.u32, 0, 0);
    if (ctx.r6.u32)
        Store32(ctx.r6.u32, t->handle);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamTaskShouldExit) { TRACE("XamTaskShouldExit"); ctx.r3.u64 = 0; }

GUEST_IMPORT(XamTaskCloseHandle)
{
    TRACE("XamTaskCloseHandle");
    kern::CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(XMsgStartIORequest)
{
    LOGI("XMsgStartIORequest(app %08X, message %08X): no handler", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = xstatus::Unsuccessful;
}

GUEST_IMPORT(XMsgStartIORequestEx)
{
    LOGI("XMsgStartIORequestEx(app %08X, message %08X): no handler", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = xstatus::Unsuccessful;
}

namespace
{
    // The system music player (app 0xFA), which titles query so a player's own
    // music can replace theirs. It is always idle here, with the title in control.
    uint32_t XmpMessage(uint32_t message, uint32_t args)
    {
        switch (message)
        {
        case 0x00070009: // XMPGetStatus(client, &state)
            Store32(Load32(args + 4), 0); // idle
            return 0;
        case 0x0007000B: // XMPGetVolume(client, &volume)
            Store32(Load32(args + 4), 0x3F800000); // 1.0f
            return 0;
        case 0x0007001B: // XMPGetPlaybackController(client, &controller, &locked)
            Store32(Load32(args + 4), 0);
            Store32(Load32(args + 8), 0);
            return 0;
        default: // play, stop, pause, volume, playlists, behavior: accepted, nothing plays
        {
            static std::vector<uint32_t> logged;
            if (std::find(logged.begin(), logged.end(), message) == logged.end())
            {
                logged.push_back(message);
                LOGI("XMP message %08X accepted (no music player)", message);
            }
            return 0;
        }
        }
    }
}

GUEST_IMPORT(XMsgInProcessCall)
{
    uint32_t app = ctx.r3.u32, message = ctx.r4.u32, args = ctx.r5.u32;
    if (app == 0xFA && args)
    {
        ctx.r3.u64 = XmpMessage(message, args);
        return;
    }
    static int logged = 0;
    if (logged++ < 32)
        LOGI("XMsgInProcessCall(app %08X, message %08X): no handler", app, message);
    ctx.r3.u64 = xstatus::Unsuccessful;
}

GUEST_IMPORT(XMsgCancelIORequest) { TRACE("XMsgCancelIORequest"); ctx.r3.u64 = 0; }

// ---- voice, sessions, gamer tiles -----------------------------------------------

GUEST_IMPORT(XamVoiceCreate)
{
    TRACE("XamVoiceCreate");
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, 0);
    ctx.r3.u64 = xerror::AccessDenied;
}

GUEST_IMPORT(XamVoiceHeadsetPresent) { TRACE("XamVoiceHeadsetPresent"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamVoiceClose) { TRACE("XamVoiceClose"); ctx.r3.u64 = 0; }
GUEST_IMPORT(XamVoiceSubmitPacket) { TRACE("XamVoiceSubmitPacket"); ctx.r3.u64 = 0; }

GUEST_IMPORT(XamSessionCreateHandle)
{
    TRACE("XamSessionCreateHandle");
    Store32(ctx.r3.u32, kern::CreateHandle(kern::HandleKind::Object, kern::NewEvent(true, false)));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamSessionRefObjByHandle)
{
    TRACE("XamSessionRefObjByHandle");
    uint32_t obj = kern::ObjectFromHandle(ctx.r3.u32);
    Store32(ctx.r4.u32, obj);
    ctx.r3.u64 = obj ? 0 : xerror::InvalidHandle;
}

GUEST_IMPORT(XamParseGamerTileKey) { TRACE("XamParseGamerTileKey"); ctx.r3.u64 = xerror::NotFound; }
GUEST_IMPORT(XamReadTileToTexture) { TRACE("XamReadTileToTexture"); ctx.r3.u64 = xerror::NotFound; }
GUEST_IMPORT(XamWriteGamerTile) { TRACE("XamWriteGamerTile"); ctx.r3.u64 = xerror::NotFound; }

// ---- input (XInput) ------------------------------------------------------------
// XINPUT_STATE: be32 packet, be16 buttons, u8 lt, u8 rt, be16 lx, ly, rx, ry

GUEST_IMPORT(XamInputGetCapabilities)
{
    TRACE("XamInputGetCapabilities");
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    if (!input::IsConnected(user))
    {
        ctx.r3.u64 = xerror::DeviceNotConnected;
        return;
    }
    XINPUT_CAPABILITIES c{};
    if (XInputGetCapabilities(user, 0, &c) != ERROR_SUCCESS)
    {
        // The keyboard standing in for player 1's pad: a full gamepad.
        c.Flags = 0;
        c.Gamepad.wButtons = 0xF3FF;
        c.Gamepad.bLeftTrigger = c.Gamepad.bRightTrigger = 0xFF;
        c.Gamepad.sThumbLX = c.Gamepad.sThumbLY = c.Gamepad.sThumbRX = c.Gamepad.sThumbRY = int16_t(0xFFC0);
        c.Vibration.wLeftMotorSpeed = c.Vibration.wRightMotorSpeed = 0xFFFF;
    }
    memset(g_base + caps, 0, 0x14);
    Store8(caps + 0, 1); // gamepad
    Store8(caps + 1, 1);
    Store16(caps + 2, c.Flags);
    Store16(caps + 4, c.Gamepad.wButtons);
    Store8(caps + 6, c.Gamepad.bLeftTrigger);
    Store8(caps + 7, c.Gamepad.bRightTrigger);
    Store16(caps + 8, uint16_t(c.Gamepad.sThumbLX));
    Store16(caps + 10, uint16_t(c.Gamepad.sThumbLY));
    Store16(caps + 12, uint16_t(c.Gamepad.sThumbRX));
    Store16(caps + 14, uint16_t(c.Gamepad.sThumbRY));
    Store16(caps + 16, c.Vibration.wLeftMotorSpeed);
    Store16(caps + 18, c.Vibration.wRightMotorSpeed);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamInputGetState)
{
    TRACE("XamInputGetState");
    uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
    XINPUT_STATE s{};
    if (!input::GetState(user, s))
    {
        ctx.r3.u64 = xerror::DeviceNotConnected;
        return;
    }
    Store32(state + 0, s.dwPacketNumber);
    Store16(state + 4, s.Gamepad.wButtons);
    Store8(state + 6, s.Gamepad.bLeftTrigger);
    Store8(state + 7, s.Gamepad.bRightTrigger);
    Store16(state + 8, uint16_t(s.Gamepad.sThumbLX));
    Store16(state + 10, uint16_t(s.Gamepad.sThumbLY));
    Store16(state + 12, uint16_t(s.Gamepad.sThumbRX));
    Store16(state + 14, uint16_t(s.Gamepad.sThumbRY));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamInputSetState)
{
    TRACE("XamInputSetState");
    uint32_t user = ctx.r3.u32, vib = ctx.r5.u32;
    if (!input::IsConnected(user))
    {
        ctx.r3.u64 = xerror::DeviceNotConnected;
        return;
    }
    input::SetVibration(user, Load16(vib), Load16(vib + 2));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamInputGetKeystrokeEx)
{
    TRACE("XamInputGetKeystrokeEx");
    ctx.r3.u64 = xerror::Empty;
}
