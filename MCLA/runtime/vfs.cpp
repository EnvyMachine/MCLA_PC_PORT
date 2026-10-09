#include "vfs.h"
#include "log.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace vfs
{
    static std::filesystem::path s_gameDir, s_userDir;
    static std::mutex s_lock;
    static std::map<std::string, std::string> s_links;               // lowercase link -> target
    static std::map<std::string, std::filesystem::path> s_mounts;    // lowercase device -> host dir

    static std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
        return s;
    }

    static std::string Normalize(std::string p)
    {
        std::replace(p.begin(), p.end(), '/', '\\');
        if (p.rfind("\\??\\", 0) == 0)
            p.erase(0, 4);
        // collapse duplicate separators
        std::string out;
        for (char c : p)
            if (!(c == '\\' && !out.empty() && out.back() == '\\'))
                out += c;
        return out;
    }

    void Init(const std::filesystem::path& gameDir, const std::filesystem::path& userDir)
    {
        s_gameDir = gameDir;
        s_userDir = userDir;
        std::filesystem::create_directories(userDir / "cache");
        std::filesystem::create_directories(userDir / "hdd");
        std::filesystem::create_directories(userDir / "content");

        s_mounts["game:"] = gameDir;
        s_mounts["d:"] = gameDir;
        s_mounts["\\device\\cdrom0"] = gameDir;
        s_mounts["cache:"] = userDir / "cache";
        s_mounts["\\device\\harddisk0\\cache0"] = userDir / "cache";
        s_mounts["hdd:"] = userDir / "hdd";
        s_mounts["\\device\\harddisk0\\partition1"] = userDir / "hdd";
        LOGI("vfs: game:=%s user=%s", gameDir.string().c_str(), userDir.string().c_str());
    }

    const std::filesystem::path& UserDir() { return s_userDir; }

    void AddLink(const std::string& link, const std::string& target)
    {
        std::lock_guard lock(s_lock);
        s_links[Lower(Normalize(link))] = Normalize(target);
        LOGI("vfs: link %s -> %s", link.c_str(), target.c_str());
    }

    void RemoveLink(const std::string& link)
    {
        std::lock_guard lock(s_lock);
        s_links.erase(Lower(Normalize(link)));
    }

    void Mount(const std::string& device, const std::filesystem::path& hostDir)
    {
        std::lock_guard lock(s_lock);
        std::string d = Lower(device);
        if (d.back() != ':')
            d += ':';
        s_mounts[d] = hostDir;
        LOGI("vfs: mount %s -> %s", d.c_str(), hostDir.string().c_str());
    }

    void Unmount(const std::string& device)
    {
        std::lock_guard lock(s_lock);
        std::string d = Lower(device);
        if (d.back() != ':')
            d += ':';
        s_mounts.erase(d);
    }

    bool Resolve(const std::string& xboxPath, std::filesystem::path& out, bool* readOnly)
    {
        std::lock_guard lock(s_lock);
        std::string path = Normalize(xboxPath);

        // Apply symbolic links (longest matching prefix), a few levels deep.
        for (int depth = 0; depth < 8; depth++)
        {
            std::string lower = Lower(path);
            const std::pair<const std::string, std::string>* best = nullptr;
            for (auto& link : s_links)
                if (lower.rfind(link.first, 0) == 0 && (lower.size() == link.first.size() || lower[link.first.size()] == '\\' || link.first.back() == ':'))
                    if (!best || link.first.size() > best->first.size())
                        best = &link;
            if (!best)
                break;
            path = best->second + path.substr(best->first.size());
        }

        std::string lower = Lower(path);
        const std::pair<const std::string, std::filesystem::path>* mount = nullptr;
        for (auto& m : s_mounts)
            if (lower.rfind(m.first, 0) == 0 && (lower.size() == m.first.size() || lower[m.first.size()] == '\\' || m.first.back() == ':'))
                if (!mount || m.first.size() > mount->first.size())
                    mount = &m;
        if (!mount)
            return false;

        std::string rest = path.substr(mount->first.size());
        while (!rest.empty() && rest.front() == '\\')
            rest.erase(0, 1);
        out = rest.empty() ? mount->second : mount->second / std::filesystem::path(rest);
        if (readOnly)
            *readOnly = mount->second == s_gameDir;
        return true;
    }

    File::~File()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
}
