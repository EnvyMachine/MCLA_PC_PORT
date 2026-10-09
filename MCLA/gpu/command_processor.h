// Xenos GPU command processor (front end only).
//
// Walks the PM4 command stream the statically linked D3D library writes to the
// primary ring buffer (and the indirect buffers it references), keeps the GPU
// register file, and executes every packet with a CPU-visible side effect: fence
// and event writes, scratch-register writeback, conditional writes, waits and
// interrupts. Draw packets are parsed and skipped for now - rendering comes later.
// Packet semantics follow Xenia's command processor.
#pragma once

#include <cstdint>

namespace gpu
{
    void InitializeRingBuffer(uint32_t physAddress, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physAddress, uint32_t blockSizeLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t context);

    // Fills the 64-dword space D3D reserves in the ring buffer for VdSwap.
    void WriteSwapPacket(uint32_t bufferAddress, uint32_t fetchConstant, uint32_t frontBufferAddress,
                         uint32_t width, uint32_t height);

    uint64_t FramesPresented();
    uint64_t DrawCount();

    // Register file access (registers are mirrored at guest 0x7FC80000 + index*4,
    // where the CPU reads and writes them directly).
    uint32_t ReadRegister(uint32_t index);
}
