#include "fxc_compiler.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3dcompiler.h>
#endif

std::vector<uint8_t> compileSm3(const std::string& hlsl, bool isPixelShader, std::string& error)
{
#ifdef _WIN32
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;

    const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main",
                                  isPixelShader ? "ps_3_0" : "vs_3_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

    std::vector<uint8_t> out;
    if (SUCCEEDED(hr) && code != nullptr)
    {
        const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        out.assign(p, p + code->GetBufferSize());
    }
    else if (errors != nullptr)
    {
        error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    }
    else
    {
        error = "D3DCompile failed with no diagnostics";
    }

    if (code)
        code->Release();
    if (errors)
        errors->Release();
    return out;
#else
    (void)hlsl;
    (void)isPixelShader;
    error = "SM3 compilation requires Windows (d3dcompiler)";
    return {};
#endif
}
