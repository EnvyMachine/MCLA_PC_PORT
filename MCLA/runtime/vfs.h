// Maps Xbox paths onto host folders and wraps host file handles.
//
//   game:, d:, \Device\Cdrom0              -> extracted disc (read-only)
//   cache:, \Device\Harddisk0\Cache0        -> <user>/cache
//   hdd:, \Device\Harddisk0\Partition1      -> <user>/hdd
//   content roots from XamContentCreate     -> <user>/content/...
// plus any symbolic links the title creates (ObCreateSymbolicLink).
#pragma once

#include "kernel.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vfs
{
    void Init(const std::filesystem::path& gameDir, const std::filesystem::path& userDir);
    const std::filesystem::path& UserDir();

    // Resolves an Xbox path. readOnly is set for disc paths.
    bool Resolve(const std::string& xboxPath, std::filesystem::path& out, bool* readOnly = nullptr);

    void AddLink(const std::string& link, const std::string& target);
    void RemoveLink(const std::string& link);
    // Mounts a host folder as a device root, e.g. "save:" for XamContentCreate.
    void Mount(const std::string& device, const std::filesystem::path& hostDir);
    void Unmount(const std::string& device);

    struct File : kern::HostObject
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::string xboxPath;
        std::filesystem::path hostPath;
        bool isDirectory = false;
        uint64_t position = 0;
        // Directory enumeration state (NtQueryDirectoryFile).
        std::vector<WIN32_FIND_DATAW> entries;
        size_t nextEntry = 0;
        bool enumerated = false;

        ~File() override;
    };
}
