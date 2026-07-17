#include "fxc_compiler.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3dcompiler.h>
#endif

// NOTE: D3DCompile runs UNLOCKED from the parallel shader loop, deliberately. d3dcompiler_47
// is documented thread-safe, and the serialization that used to live here was a misdiagnosis:
// the intermittent 0xC0000409 crash it was meant to cure reproduced with the whole corpus
// loop forced to std::execution::seq and every compile mutexed -- fully serial -- so whatever
// causes it, it is not a compiler data race. Serializing cost 10+ minutes per cache build.

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

        const HRESULT hr =
            D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main",
                       isPixelShader ? "ps_3_0" : "vs_3_0", flags, 0, &code, &errors);

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
