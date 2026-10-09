// File system imports, backed by host files through the vfs.
#include "imports.h"
#include "../runtime/memory_watch.h"
#include "../runtime/vfs.h"

#include <windows.h>

#include <algorithm>

namespace
{
    constexpr uint32_t kFileAttributeDirectory = 0x10;
    constexpr uint32_t kFileAttributeNormal = 0x80;
    constexpr uint32_t kFileAttributeReadOnly = 0x01;
    constexpr uint32_t kFileDirectoryFile = 0x1; // create option
    constexpr uint32_t kFileNonDirectoryFile = 0x40;

    // IO_STATUS_BLOCK.information values
    constexpr uint32_t kFileSuperseded = 0, kFileOpened = 1, kFileCreated = 2, kFileOverwritten = 3;

    void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t information)
    {
        if (iosb)
        {
            Store32(iosb + 0, status);
            Store32(iosb + 4, information);
        }
    }

    uint64_t FileTimeValue(const FILETIME& ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

    std::shared_ptr<vfs::File> GetFile(uint32_t handle)
    {
        kern::HandleEntry e;
        if (!kern::LookupHandle(handle, e) || e.kind != kern::HandleKind::File)
            return {};
        return std::static_pointer_cast<vfs::File>(e.host);
    }

    uint32_t StatusFromWin32(DWORD err)
    {
        switch (err)
        {
        case ERROR_FILE_NOT_FOUND: return xstatus::ObjectNameNotFound;
        case ERROR_PATH_NOT_FOUND: return xstatus::ObjectPathNotFound;
        case ERROR_ACCESS_DENIED: return xstatus::AccessDenied;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return xstatus::ObjectNameCollision;
        case ERROR_HANDLE_EOF: return xstatus::EndOfFile;
        default: return xstatus::Unsuccessful;
        }
    }

    // Path from OBJECT_ATTRIBUTES, relative to root_directory if one is given.
    std::string FullPath(uint32_t objectAttributes)
    {
        std::string name = ReadObjectName(objectAttributes);
        uint32_t root = objectAttributes ? Load32(objectAttributes) : 0;
        if (root)
            if (auto dir = GetFile(root))
                return dir->xboxPath + (name.empty() || name[0] == '\\' ? "" : "\\") + name;
        return name;
    }

    bool HostAttributes(const std::filesystem::path& p, WIN32_FILE_ATTRIBUTE_DATA& data)
    {
        return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data) != 0;
    }

    void WriteNetworkOpenInfo(uint32_t out, const WIN32_FILE_ATTRIBUTE_DATA& d, bool readOnlyDevice)
    {
        uint64_t size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        Store64(out + 0x00, FileTimeValue(d.ftCreationTime));
        Store64(out + 0x08, FileTimeValue(d.ftLastAccessTime));
        Store64(out + 0x10, FileTimeValue(d.ftLastWriteTime));
        Store64(out + 0x18, FileTimeValue(d.ftLastWriteTime));
        Store64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
        Store64(out + 0x28, size);
        uint32_t attrs = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? kFileAttributeDirectory : kFileAttributeNormal;
        if (readOnlyDevice)
            attrs |= kFileAttributeReadOnly;
        Store32(out + 0x30, attrs);
        Store32(out + 0x34, 0);
    }

    uint32_t OpenFile(uint32_t pHandle, uint32_t access, uint32_t objectAttributes, uint32_t iosb,
                      uint32_t disposition, uint32_t options, const char* api)
    {
        std::string xboxPath = FullPath(objectAttributes);
        std::filesystem::path hostPath;
        bool readOnly = false;
        if (!vfs::Resolve(xboxPath, hostPath, &readOnly))
        {
            LOGI("%s: unknown device for '%s'", api, xboxPath.c_str());
            SetIoStatus(iosb, xstatus::ObjectPathNotFound, 0);
            return xstatus::ObjectPathNotFound;
        }

        const bool wantsWrite = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | 0x40000000)) != 0;
        std::error_code ec;
        bool exists = std::filesystem::exists(hostPath, ec);
        bool isDir = exists && std::filesystem::is_directory(hostPath, ec);

        if (readOnly && (disposition != 1 /*FILE_OPEN*/ && disposition != 3 /*FILE_OPEN_IF*/ || (disposition == 3 && !exists)))
        {
            uint32_t s = exists ? xstatus::AccessDenied : xstatus::ObjectNameNotFound;
            SetIoStatus(iosb, s, 0);
            return s;
        }

        auto file = std::make_shared<vfs::File>();
        file->xboxPath = xboxPath;
        file->hostPath = hostPath;
        uint32_t information = kFileOpened;

        if (isDir || (options & kFileDirectoryFile))
        {
            if (!exists)
            {
                if (disposition == 2 /*FILE_CREATE*/ || disposition == 3 /*FILE_OPEN_IF*/)
                {
                    std::filesystem::create_directories(hostPath, ec);
                    information = kFileCreated;
                }
                else
                {
                    if (g_traceImports)
                        LOGI("%s: '%s' not found", api, xboxPath.c_str());
                    SetIoStatus(iosb, xstatus::ObjectNameNotFound, 0);
                    return xstatus::ObjectNameNotFound;
                }
            }
            else if (disposition == 2)
            {
                SetIoStatus(iosb, xstatus::ObjectNameCollision, 0);
                return xstatus::ObjectNameCollision;
            }
            if (options & kFileNonDirectoryFile)
            {
                SetIoStatus(iosb, xstatus::FileIsADirectory, 0);
                return xstatus::FileIsADirectory;
            }
            file->isDirectory = true;
        }
        else
        {
            DWORD creation;
            switch (disposition)
            {
            case 0: creation = CREATE_ALWAYS; information = exists ? kFileSuperseded : kFileCreated; break;
            case 1: creation = OPEN_EXISTING; break;
            case 2: creation = CREATE_NEW; information = kFileCreated; break;
            case 3: creation = OPEN_ALWAYS; information = exists ? kFileOpened : kFileCreated; break;
            case 4: creation = TRUNCATE_EXISTING; information = kFileOverwritten; break;
            case 5: creation = CREATE_ALWAYS; information = exists ? kFileOverwritten : kFileCreated; break;
            default: creation = OPEN_EXISTING; break;
            }
            if (creation != OPEN_EXISTING)
                std::filesystem::create_directories(hostPath.parent_path(), ec);
            DWORD hostAccess = GENERIC_READ | ((wantsWrite || creation != OPEN_EXISTING) && !readOnly ? GENERIC_WRITE : 0);
            file->handle = CreateFileW(hostPath.c_str(), hostAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file->handle == INVALID_HANDLE_VALUE)
            {
                uint32_t s = StatusFromWin32(GetLastError());
                if (g_traceImports || s != xstatus::ObjectNameNotFound)
                    LOGI("%s: '%s' failed (%08X)", api, xboxPath.c_str(), s);
                SetIoStatus(iosb, s, 0);
                return s;
            }
        }

        // File objects are waitable (signalled when I/O completes); I/O here is synchronous.
        uint32_t object = kern::NewEvent(true, true);
        uint32_t handle = kern::CreateHandle(kern::HandleKind::File, object, file);
        Store32(pHandle, handle);
        SetIoStatus(iosb, xstatus::Success, information);
        LOGI("%s: '%s' -> %08X%s", api, xboxPath.c_str(), handle, file->isDirectory ? " (dir)" : "");
        return xstatus::Success;
    }

    // Byte offset argument: null or -2 (FILE_USE_FILE_POINTER_POSITION) = current position.
    uint64_t ResolveOffset(vfs::File& f, uint32_t pOffset)
    {
        if (!pOffset)
            return f.position;
        uint64_t off = Load64(pOffset);
        return off == 0xFFFFFFFFFFFFFFFEull ? f.position : off;
    }

    void FinishIo(PPCContext& ctx, uint32_t event, uint32_t apcRoutine, uint32_t iosb, uint32_t status, uint32_t information)
    {
        SetIoStatus(iosb, status, information);
        if (event)
            if (uint32_t obj = kern::ObjectFromHandle(event))
                kern::SetEvent(obj);
        if (apcRoutine)
            LOGI("I/O completion APC %08X not delivered", apcRoutine);
        ctx.r3.u64 = status;
    }
}

GUEST_IMPORT(NtCreateFile)
{
    TRACE("NtCreateFile");
    uint32_t options = Arg(ctx, 8);
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options, "NtCreateFile");
}

GUEST_IMPORT(NtOpenFile)
{
    TRACE("NtOpenFile");
    // NtOpenFile(handle*, access, object_attributes, iosb, share, open_options)
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32, "NtOpenFile");
}

GUEST_IMPORT(NtReadFile)
{
    TRACE("NtReadFile");
    uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, iosb = ctx.r7.u32;
    uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    auto f = GetFile(handle);
    if (!f || f->isDirectory)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    uint64_t offset = ResolveOffset(*f, pOffset);
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD read = 0;
    mem::PrepareHostWrite(buffer, length);
    BOOL ok = ReadFile(f->handle, g_base + buffer, length, &read, &ov);
    if (!ok && GetLastError() != ERROR_HANDLE_EOF)
    {
        FinishIo(ctx, event, apc, iosb, StatusFromWin32(GetLastError()), 0);
        return;
    }
    f->position = offset + read;
    FinishIo(ctx, event, apc, iosb, (read == 0 && length) ? xstatus::EndOfFile : xstatus::Success, read);
}

GUEST_IMPORT(NtReadFileScatter)
{
    TRACE("NtReadFileScatter");
    // segments: array of be32 page addresses (FILE_SEGMENT_ELEMENT), each 4 KB.
    uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, iosb = ctx.r7.u32;
    uint32_t segments = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    auto f = GetFile(handle);
    if (!f || f->isDirectory)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    uint64_t offset = ResolveOffset(*f, pOffset);
    uint32_t total = 0;
    for (uint32_t i = 0; total < length; i++)
    {
        uint32_t page = Load32(segments + i * 4);
        uint32_t chunk = std::min(0x1000u, length - total);
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset + total);
        ov.OffsetHigh = DWORD((offset + total) >> 32);
        DWORD read = 0;
        mem::PrepareHostWrite(page, chunk);
        ReadFile(f->handle, g_base + page, chunk, &read, &ov);
        total += read;
        if (read < chunk)
            break;
    }
    f->position = offset + total;
    FinishIo(ctx, event, apc, iosb, (total == 0 && length) ? xstatus::EndOfFile : xstatus::Success, total);
}

GUEST_IMPORT(NtWriteFile)
{
    TRACE("NtWriteFile");
    uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, iosb = ctx.r7.u32;
    uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    auto f = GetFile(handle);
    if (!f || f->isDirectory)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    uint64_t offset = ResolveOffset(*f, pOffset);
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD written = 0;
    if (!WriteFile(f->handle, g_base + buffer, length, &written, &ov))
    {
        FinishIo(ctx, event, apc, iosb, StatusFromWin32(GetLastError()), 0);
        return;
    }
    f->position = offset + written;
    FinishIo(ctx, event, apc, iosb, xstatus::Success, written);
}

GUEST_IMPORT(NtFlushBuffersFile)
{
    TRACE("NtFlushBuffersFile");
    if (auto f = GetFile(ctx.r3.u32); f && f->handle != INVALID_HANDLE_VALUE)
        FlushFileBuffers(f->handle);
    SetIoStatus(ctx.r4.u32, xstatus::Success, 0);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(NtQueryInformationFile)
{
    TRACE("NtQueryInformationFile");
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    auto f = GetFile(ctx.r3.u32);
    if (!f)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    HostAttributes(f->hostPath, data);
    uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    uint32_t written = 0, status = xstatus::Success;
    switch (infoClass)
    {
    case 4: // FileBasicInformation
        Store64(info + 0x00, FileTimeValue(data.ftCreationTime));
        Store64(info + 0x08, FileTimeValue(data.ftLastAccessTime));
        Store64(info + 0x10, FileTimeValue(data.ftLastWriteTime));
        Store64(info + 0x18, FileTimeValue(data.ftLastWriteTime));
        Store64(info + 0x20, f->isDirectory ? kFileAttributeDirectory : kFileAttributeNormal);
        written = 0x28;
        break;
    case 5: // FileStandardInformation: alloc size, eof, links, delete_pending, directory
        Store64(info + 0x00, (size + 0xFFF) & ~0xFFFull);
        Store64(info + 0x08, size);
        Store32(info + 0x10, 1);
        Store8(info + 0x14, 0);
        Store8(info + 0x15, f->isDirectory ? 1 : 0);
        written = 0x18;
        break;
    case 6: // FileInternalInformation
        Store64(info, std::hash<std::string>{}(f->xboxPath));
        written = 8;
        break;
    case 14: // FilePositionInformation
        Store64(info, f->position);
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(info, data, false);
        written = 0x38;
        break;
    case 17: // FileAlignmentInformation
        Store32(info, 0);
        written = 4;
        break;
    case 27: // FileXctdCompressionInformation
        Store32(info, 0);
        written = 4;
        break;
    case 26: // FileSectorInformation
        Store32(info, 0);
        written = 4;
        break;
    default:
        LOGI("NtQueryInformationFile: unhandled class %u on '%s'", infoClass, f->xboxPath.c_str());
        status = xstatus::InvalidParameter;
        break;
    }
    if (written > length && status == xstatus::Success)
        status = xstatus::InfoLengthMismatch;
    SetIoStatus(iosb, status, written);
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtSetInformationFile)
{
    TRACE("NtSetInformationFile");
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    auto f = GetFile(ctx.r3.u32);
    if (!f)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    uint32_t status = xstatus::Success;
    switch (infoClass)
    {
    case 14: // FilePositionInformation
        f->position = Load64(info);
        break;
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
    {
        LARGE_INTEGER li;
        li.QuadPart = int64_t(Load64(info));
        if (f->handle == INVALID_HANDLE_VALUE || !SetFilePointerEx(f->handle, li, nullptr, FILE_BEGIN) || !SetEndOfFile(f->handle))
            status = xstatus::AccessDenied;
        break;
    }
    case 13: // FileDispositionInformation (delete on close)
        if (Load8(info))
        {
            std::error_code ec;
            if (f->handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(f->handle);
                f->handle = INVALID_HANDLE_VALUE;
            }
            std::filesystem::remove(f->hostPath, ec);
        }
        break;
    case 4: // FileBasicInformation (times/attributes): ignore
        break;
    default:
        LOGI("NtSetInformationFile: unhandled class %u on '%s'", infoClass, f->xboxPath.c_str());
        status = xstatus::InvalidParameter;
        break;
    }
    SetIoStatus(iosb, status, 0);
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtQueryFullAttributesFile)
{
    TRACE("NtQueryFullAttributesFile");
    std::string xboxPath = FullPath(ctx.r3.u32);
    std::filesystem::path hostPath;
    bool readOnly = false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!vfs::Resolve(xboxPath, hostPath, &readOnly) || !HostAttributes(hostPath, data))
    {
        if (g_traceImports)
            LOGI("NtQueryFullAttributesFile: '%s' not found", xboxPath.c_str());
        ctx.r3.u64 = xstatus::ObjectNameNotFound;
        return;
    }
    WriteNetworkOpenInfo(ctx.r4.u32, data, readOnly);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    TRACE("NtQueryVolumeInformationFile");
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    auto f = GetFile(ctx.r3.u32);
    if (!f)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    uint32_t written = 0, status = xstatus::Success;
    switch (infoClass)
    {
    case 1: // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
        Store64(info + 0, 0);
        Store32(info + 8, 0x12345678);
        Store32(info + 12, 0);
        Store8(info + 16, 0);
        written = 0x18;
        break;
    case 3: // FileFsSizeInformation: total units, free units, sectors/unit, bytes/sector
        Store64(info + 0, 0x100000);
        Store64(info + 8, 0x80000);
        Store32(info + 16, 8);
        Store32(info + 20, 0x200);
        written = 0x18;
        break;
    case 5: // FileFsAttributeInformation: attributes, max name length, name length, name
    {
        const char name[] = "FATX";
        Store32(info + 0, 0);
        Store32(info + 4, 255);
        Store32(info + 8, 4);
        memcpy(g_base + info + 12, name, std::min<uint32_t>(4, length > 12 ? length - 12 : 0));
        written = 16;
        break;
    }
    default:
        LOGI("NtQueryVolumeInformationFile: unhandled class %u", infoClass);
        status = xstatus::InvalidParameter;
        break;
    }
    SetIoStatus(iosb, status, written);
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtQueryDirectoryFile)
{
    TRACE("NtQueryDirectoryFile");
    // (handle, event, apc, apc_ctx, iosb, info, length, file_mask, restart_scan)
    uint32_t iosb = ctx.r7.u32, info = ctx.r8.u32, length = ctx.r9.u32, mask = ctx.r10.u32;
    bool restart = Arg(ctx, 8) != 0;
    auto f = GetFile(ctx.r3.u32);
    if (!f || !f->isDirectory)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    if (!f->enumerated || restart)
    {
        std::string pattern = mask ? ReadAnsiString(mask) : "*";
        if (pattern.empty())
            pattern = "*";
        f->entries.clear();
        f->nextEntry = 0;
        WIN32_FIND_DATAW fd;
        std::wstring wpattern(pattern.begin(), pattern.end());
        HANDLE find = FindFirstFileW((f->hostPath / wpattern).c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                    f->entries.push_back(fd);
            while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        f->enumerated = true;
        if (f->entries.empty())
        {
            SetIoStatus(iosb, xstatus::NoSuchFile, 0);
            ctx.r3.u64 = xstatus::NoSuchFile;
            return;
        }
    }
    if (f->nextEntry >= f->entries.size())
    {
        SetIoStatus(iosb, xstatus::NoMoreFiles, 0);
        ctx.r3.u64 = xstatus::NoMoreFiles;
        return;
    }

    // X_FILE_DIRECTORY_INFORMATION, one entry per call.
    const WIN32_FIND_DATAW& fd = f->entries[f->nextEntry++];
    std::string name;
    for (const wchar_t* c = fd.cFileName; *c; c++)
        name += char(*c < 0x80 ? *c : '_');
    uint32_t needed = 0x40 + uint32_t(name.size());
    if (needed > length)
    {
        f->nextEntry--;
        SetIoStatus(iosb, xstatus::BufferOverflow, 0);
        ctx.r3.u64 = xstatus::BufferOverflow;
        return;
    }
    uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    Store32(info + 0x00, 0); // next_entry_offset
    Store32(info + 0x04, uint32_t(f->nextEntry));
    Store64(info + 0x08, FileTimeValue(fd.ftCreationTime));
    Store64(info + 0x10, FileTimeValue(fd.ftLastAccessTime));
    Store64(info + 0x18, FileTimeValue(fd.ftLastWriteTime));
    Store64(info + 0x20, FileTimeValue(fd.ftLastWriteTime));
    Store64(info + 0x28, size);
    Store64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
    Store32(info + 0x38, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? kFileAttributeDirectory : kFileAttributeNormal);
    Store32(info + 0x3C, uint32_t(name.size()));
    memcpy(g_base + info + 0x40, name.data(), name.size());
    SetIoStatus(iosb, xstatus::Success, needed);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(NtDeviceIoControlFile)
{
    TRACE("NtDeviceIoControlFile");
    LOGI("NtDeviceIoControlFile: handle %08X ioctl %08X (unsupported)", ctx.r3.u32, ctx.r8.u32);
    ctx.r3.u64 = xstatus::NotSupported;
}

GUEST_IMPORT(ObCreateSymbolicLink)
{
    TRACE("ObCreateSymbolicLink");
    vfs::AddLink(ReadAnsiString(ctx.r3.u32), ReadAnsiString(ctx.r4.u32));
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(ObDeleteSymbolicLink)
{
    TRACE("ObDeleteSymbolicLink");
    vfs::RemoveLink(ReadAnsiString(ctx.r3.u32));
    ctx.r3.u64 = xstatus::Success;
}
