// Loads default.xex into guest memory, builds the indirect-call table, and
// resolves imported kernel variables.
#pragma once

#include <cstdint>
#include <filesystem>

struct XexInfo
{
    uint32_t entryPoint = 0;
    uint32_t imageBase = 0, imageSize = 0;
    uint32_t header = 0, headerSize = 0; // guest copy of the XEX header
    uint32_t ldrEntry = 0;               // X_LDR_DATA_TABLE_ENTRY ("module handle")
    uint32_t titleId = 0;
    uint32_t systemFlags = 0;
    uint32_t stackSize = 0;

    // Kernel variables exported to the title.
    uint32_t timeStampBundle = 0;
    uint32_t vdGlobalDevice = 0;
    uint32_t vdHsioCalibrationLock = 0;
};

extern XexInfo g_xex;

bool LoadXex(const std::filesystem::path& path);

// RtlImageXexHeaderField semantics on a guest XEX header: inline values are
// returned directly, everything else as a guest pointer to the header data.
uint32_t XexHeaderField(uint32_t header, uint32_t key);
