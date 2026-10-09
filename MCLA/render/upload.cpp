#include "upload.h"
#include "device.h"
#include "../runtime/log.h"

#include <vector>

namespace render
{
    namespace
    {
        constexpr uint64_t kChunkSize = 64ull << 20;

        struct Chunk
        {
            ComPtr<ID3D12Resource> resource;
            uint8_t* cpu = nullptr;
            uint64_t size = 0;
            uint64_t used = 0;
            uint64_t lastFrame = 0; // last frame that allocated from it
        };

        std::vector<Chunk> s_chunks;
        Chunk* s_current = nullptr;

        Chunk* NewChunk(uint64_t size)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Chunk c;
            if (FAILED(Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                         nullptr, IID_PPV_ARGS(&c.resource))))
            {
                LOGI("render: could not create a %llu MB upload chunk", (unsigned long long)(size >> 20));
                LogFlush();
                ExitProcess(1);
            }
            D3D12_RANGE none{ 0, 0 };
            c.resource->Map(0, &none, reinterpret_cast<void**>(&c.cpu));
            c.size = size;
            s_chunks.push_back(std::move(c));
            LOGI("render: upload chunk %zu (%llu MB)", s_chunks.size(), (unsigned long long)(size >> 20));
            return &s_chunks.back();
        }
    }

    UploadSpace AllocateUpload(uint64_t size, uint64_t alignment)
    {
        uint64_t frame = FrameNumber();
        auto fits = [&](Chunk* c) { return ((c->used + alignment - 1) & ~(alignment - 1)) + size <= c->size; };
        if (!s_current || !fits(s_current))
        {
            s_current = nullptr;
            uint64_t completed = CompletedFrame();
            for (Chunk& c : s_chunks)
            {
                if (c.lastFrame <= completed && c.size >= size)
                {
                    c.used = 0;
                    s_current = &c;
                    break;
                }
            }
            if (!s_current)
            {
                // Pointers into s_chunks stay valid only until it grows; s_current
                // is re-pointed right here.
                s_current = NewChunk(size > kChunkSize ? size : kChunkSize);
            }
        }
        Chunk* c = s_current;
        c->used = (c->used + alignment - 1) & ~(alignment - 1);
        UploadSpace space;
        space.cpu = c->cpu + c->used;
        space.resource = c->resource.Get();
        space.offset = c->used;
        space.gpu = c->resource->GetGPUVirtualAddress() + c->used;
        c->used += size;
        c->lastFrame = frame;
        return space;
    }
}
