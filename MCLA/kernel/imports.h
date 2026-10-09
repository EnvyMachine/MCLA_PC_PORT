// Common includes for kernel/XAM import implementations.
#pragma once

#include "../runtime/guest.h"
#include "../runtime/kernel.h"
#include "../runtime/loader.h"
#include "../runtime/log.h"
#include "../runtime/memory.h"
#include "../runtime/thread.h"

#include <string>

// X_ANSI_STRING { be16 length; be16 maximum_length; be32 buffer; }
inline std::string ReadAnsiString(uint32_t ansiString)
{
    if (!ansiString)
        return {};
    uint16_t len = Load16(ansiString);
    uint32_t buf = Load32(ansiString + 4);
    return buf ? std::string(GuestPtr<char>(buf), len) : std::string();
}

// X_OBJECT_ATTRIBUTES { be32 root_directory; be32 name (X_ANSI_STRING*); be32 attributes; }
inline std::string ReadObjectName(uint32_t objectAttributes)
{
    return objectAttributes ? ReadAnsiString(Load32(objectAttributes + 4)) : std::string();
}

// Completes an XOVERLAPPED: result, length, and signals its event if any.
void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);
