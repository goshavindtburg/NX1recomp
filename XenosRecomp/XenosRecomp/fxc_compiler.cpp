#include "fxc_compiler.h"

#include <mutex>

#ifdef _WIN32
#include <Windows.h>
#include <d3dcompiler.h>
#endif

#ifdef _WIN32
// d3dcompiler is not safe to call concurrently. The shader corpus is compiled from a parallel
// for_each, and an unsynchronized D3DCompile corrupts memory: the tool died with an access
// violation or a blown stack cookie (0xC0000409) at a point that moved with the thread
// schedule -- which is why it appeared to depend on the input set and on unrelated flags.
// The DXC path next door is serialized for the same reason (nx1DxcCompileMutex in main.cpp).
static std::mutex& sm3CompileMutex()
{
    static std::mutex mutex;
    return mutex;
}
#endif

std::vector<uint8_t> compileSm3(const std::string& hlsl, bool isPixelShader, std::string& error)
{
#ifdef _WIN32
    // Xenos predication (`if (p0)`) becomes a chain of predicated blocks in the emitted HLSL.
    // fxc flattens those into `cmp` selects by default, which keeps every branch's values live
    // at once -- and the densest shaders then exhaust ps_3_0's 32 temp registers ("X4505:
    // maximum temp register index exceeded"). Asking for real flow control instead lets the
    // register allocator reuse temps across branches.
    //
    // Flow control is not free (a dynamic branch costs more than a select), so it is a
    // *fallback*, not the default: the corpus compiles as-is and only the shaders that would
    // otherwise be dropped entirely pay for it.
    const UINT passes[] = {
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PREFER_FLOW_CONTROL,
    };

    for (const UINT flags : passes)
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;

        HRESULT hr;
        {
            std::lock_guard lock(sm3CompileMutex());
            hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main",
                            isPixelShader ? "ps_3_0" : "vs_3_0", flags, 0, &code, &errors);
        }

        std::vector<uint8_t> out;
        if (SUCCEEDED(hr) && code != nullptr)
        {
            const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
            out.assign(p, p + code->GetBufferSize());
        }
        else if (errors != nullptr)
        {
            error.assign(static_cast<const char*>(errors->GetBufferPointer()),
                         errors->GetBufferSize());
        }
        else
        {
            error = "D3DCompile failed with no diagnostics";
        }

        if (code)
            code->Release();
        if (errors)
            errors->Release();

        if (!out.empty())
        {
            error.clear();
            return out;
        }
    }
    return {};
#else
    (void)hlsl;
    (void)isPixelShader;
    error = "SM3 compilation requires Windows (d3dcompiler)";
    return {};
#endif
}
