#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <mutex>
#include <vector>

#include "fxc_compiler.h"
#include "sm3_transform.h"

#include <atomic>
#include <cstring>
#include <mutex>

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    auto data = std::make_unique<uint8_t[]>(fileSize);
    fread(data.get(), 1, fileSize, file);
    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    FILE* file = fopen(filePath, "wb");
    fwrite(data, 1, dataSize, file);
    fclose(file);
}

static bool endsWith(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool nx1VerboseShaderCacheLog()
{
    return std::getenv("NX1_XENOSRECOMP_VERBOSE") != nullptr;
}

static std::mutex& nx1DxcCompileMutex()
{
    static std::mutex mutex;
    return mutex;
}

static bool tryGetNx1RawShaderInfo(const std::filesystem::path& path, bool& isPixelShader,
    uint64_t& ucodeHash)
{
    const std::string name = path.filename().string();
    constexpr std::string_view prefix = "shader_";
    constexpr std::string_view infix = ".ucode.bin.";
    constexpr size_t hashLength = 16;

    if (name.size() != prefix.size() + hashLength + infix.size() + 4 ||
        name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(prefix.size() + hashLength, infix.size(), infix) != 0)
    {
        return false;
    }

    const std::string_view stage(name.data() + prefix.size() + hashLength + infix.size(), 4);
    if (stage == "frag")
    {
        isPixelShader = true;
    }
    else if (stage == "vert")
    {
        isPixelShader = false;
    }
    else
    {
        return false;
    }

    char hashText[hashLength + 1] = {};
    std::memcpy(hashText, name.data() + prefix.size(), hashLength);
    char* end = nullptr;
    ucodeHash = std::strtoull(hashText, &end, 16);
    return end == hashText + hashLength;
}

static void appendU32BE(std::vector<uint8_t>& data, uint32_t value)
{
    data.push_back(uint8_t(value >> 24));
    data.push_back(uint8_t(value >> 16));
    data.push_back(uint8_t(value >> 8));
    data.push_back(uint8_t(value));
}

static void writeU32BE(std::vector<uint8_t>& data, size_t offset, uint32_t value)
{
    data[offset + 0] = uint8_t(value >> 24);
    data[offset + 1] = uint8_t(value >> 16);
    data[offset + 2] = uint8_t(value >> 8);
    data[offset + 3] = uint8_t(value);
}

static void alignTo4(std::vector<uint8_t>& data)
{
    while ((data.size() & 3) != 0)
        data.push_back(0);
}

struct Nx1RawConstant
{
    std::string name;
    RegisterSet registerSet;
    uint16_t registerIndex;
    uint16_t registerCount;
};

static void writeConstantInfo(std::vector<uint8_t>& data, size_t offset, uint32_t nameOffset,
    RegisterSet registerSet, uint16_t registerIndex, uint16_t registerCount)
{
    writeU32BE(data, offset, nameOffset);
    data[offset + 4] = uint8_t(uint16_t(registerSet) >> 8);
    data[offset + 5] = uint8_t(uint16_t(registerSet));
    data[offset + 6] = uint8_t(registerIndex >> 8);
    data[offset + 7] = uint8_t(registerIndex);
    data[offset + 8] = uint8_t(registerCount >> 8);
    data[offset + 9] = uint8_t(registerCount);
    data[offset + 10] = 0;
    data[offset + 11] = 0;
    writeU32BE(data, offset + 12, 0);
    writeU32BE(data, offset + 16, 0);
}

static std::vector<uint8_t> buildNx1RawConstantTable(bool isPixelShader)
{
    std::vector<Nx1RawConstant> constants;
    constants.push_back({ "c", RegisterSet::Float4, 0, 256 });

    for (uint16_t i = 0; i < 16; ++i)
        constants.push_back({ fmt::format("b{}", i), RegisterSet::Bool, i, 1 });

    for (uint16_t i = 0; i < 32; ++i)
        constants.push_back({ fmt::format("i{}", i), RegisterSet::Int4, i, 1 });

    // Xbox 360 texture fetch constants are typically in the low sampler range.
    // This broad declaration keeps raw NX1 pixel shader HLSL readable before we
    // wire precise runtime reflection into the native renderer.
    for (uint16_t i = 0; i < 16; ++i)
        constants.push_back({ fmt::format("s{}", i), RegisterSet::Sampler, i, 1 });

    std::vector<uint8_t> data;
    appendU32BE(data, 0); // ConstantTableContainer::size, filled below.

    const size_t tableOffset = data.size();
    appendU32BE(data, 0); // ConstantTable::size, filled below.
    appendU32BE(data, 0); // creator
    appendU32BE(data, isPixelShader ? 0xFFFF0300 : 0xFFFE0300);
    appendU32BE(data, uint32_t(constants.size()));
    appendU32BE(data, uint32_t(sizeof(ConstantTable)));
    appendU32BE(data, 0); // flags
    appendU32BE(data, 0); // target

    const size_t constantInfoOffset = data.size();
    data.resize(data.size() + constants.size() * sizeof(ConstantInfo));

    for (size_t i = 0; i < constants.size(); ++i)
    {
        const uint32_t nameOffset = uint32_t(data.size() - tableOffset);
        data.insert(data.end(), constants[i].name.begin(), constants[i].name.end());
        data.push_back(0);
        writeConstantInfo(data, constantInfoOffset + i * sizeof(ConstantInfo), nameOffset,
            constants[i].registerSet, constants[i].registerIndex, constants[i].registerCount);
    }

    alignTo4(data);
    writeU32BE(data, 0, uint32_t(data.size()));
    writeU32BE(data, tableOffset, uint32_t(data.size() - tableOffset));
    return data;
}

static uint32_t makeInterpolator(DeclUsage usage, uint32_t usageIndex, uint32_t reg)
{
    return (usageIndex & 0xF) | ((uint32_t(usage) & 0xF) << 4) | ((reg & 0xF) << 8);
}

struct Nx1RawVertexElement
{
    uint32_t address;
    DeclUsage usage;
    uint32_t usageIndex;
};

static uint32_t makeVertexElement(uint32_t address, DeclUsage usage, uint32_t usageIndex)
{
    return (address & 0xFFF) | ((uint32_t(usage) & 0xF) << 12) | ((usageIndex & 0xF) << 16);
}

static bool parseDeclUsage(const char* name, DeclUsage& usage)
{
    if (strcmp(name, "Position") == 0)
        usage = DeclUsage::Position;
    else if (strcmp(name, "BlendWeight") == 0)
        usage = DeclUsage::BlendWeight;
    else if (strcmp(name, "BlendIndices") == 0)
        usage = DeclUsage::BlendIndices;
    else if (strcmp(name, "Normal") == 0)
        usage = DeclUsage::Normal;
    else if (strcmp(name, "PointSize") == 0)
        usage = DeclUsage::PointSize;
    else if (strcmp(name, "TexCoord") == 0)
        usage = DeclUsage::TexCoord;
    else if (strcmp(name, "Tangent") == 0)
        usage = DeclUsage::Tangent;
    else if (strcmp(name, "Binormal") == 0)
        usage = DeclUsage::Binormal;
    else if (strcmp(name, "TessFactor") == 0)
        usage = DeclUsage::TessFactor;
    else if (strcmp(name, "PositionT") == 0)
        usage = DeclUsage::PositionT;
    else if (strcmp(name, "Color") == 0)
        usage = DeclUsage::Color;
    else if (strcmp(name, "Fog") == 0)
        usage = DeclUsage::Fog;
    else if (strcmp(name, "Depth") == 0)
        usage = DeclUsage::Depth;
    else if (strcmp(name, "Sample") == 0)
        usage = DeclUsage::Sample;
    else
        return false;

    return true;
}

static void readNx1RawVertexFetchMetadata(const char* inputPath, std::vector<Nx1RawVertexElement>& vertexElements)
{
    std::string metadataPath = inputPath;
    metadataPath += ".nx1_vfetch";

    FILE* file = fopen(metadataPath.c_str(), "r");
    if (file == nullptr)
        return;

    char line[512];
    while (fgets(line, sizeof(line), file) != nullptr)
    {
        uint32_t address = 0;
        char usageName[64] = {};
        uint32_t usageIndex = 0;
        if (sscanf(line, "vfetch %u %63s %u", &address, usageName, &usageIndex) != 3)
            continue;

        DeclUsage usage;
        if (!parseDeclUsage(usageName, usage))
            continue;

        vertexElements.push_back({ address, usage, usageIndex });
    }

    fclose(file);
}

struct Nx1RawVfetchDecodeElement
{
    std::string usageName;
    uint32_t usageIndex = 0;
    uint32_t fetchConstant = 0;
    int32_t offsetWords = 0;
    uint32_t strideWords = 0;
    uint32_t format = 0;
    uint32_t usedMask = 0;
    bool isSigned = false;
    bool isInteger = false;
    std::string variableName;
    std::string typeName;
};

static std::string trimCopy(std::string_view value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace(uint8_t(value[first])))
        ++first;
    size_t last = value.size();
    while (last > first && std::isspace(uint8_t(value[last - 1])))
        --last;
    return std::string(value.substr(first, last - first));
}

static void readNx1RawVertexFetchDecodeMetadata(const char* inputPath,
    std::vector<Nx1RawVfetchDecodeElement>& elements)
{
    std::string metadataPath = inputPath;
    metadataPath += ".nx1_vfetch";

    FILE* file = fopen(metadataPath.c_str(), "r");
    if (file == nullptr)
        return;

    char line[512];
    while (fgets(line, sizeof(line), file) != nullptr)
    {
        Nx1RawVfetchDecodeElement element;
        uint32_t address = 0;
        uint32_t bindingIndex = 0;
        uint32_t resultRegister = 0;
        uint32_t signedFlag = 0;
        uint32_t integerFlag = 0;
        uint32_t roundedFlag = 0;
        uint32_t miniFlag = 0;
        char usageName[64] = {};
        if (sscanf(line, "vfetch %u %63s %u %u %u %d %u %u %u %u %u %u %u %u",
            &address, usageName, &element.usageIndex, &element.fetchConstant, &bindingIndex,
            &element.offsetWords, &element.strideWords, &element.format, &element.usedMask,
            &resultRegister, &signedFlag, &integerFlag, &roundedFlag, &miniFlag) != 14)
        {
            continue;
        }

        element.usageName = usageName;
        element.isSigned = signedFlag != 0;
        element.isInteger = integerFlag != 0;
        element.variableName = fmt::format("i{}{}", element.usageName, element.usageIndex);
        elements.push_back(std::move(element));
    }

    fclose(file);
}

static bool nx1FindInputType(std::string_view source, const std::string& variableName,
    std::string& typeName)
{
    size_t lineStart = 0;
    while (lineStart < source.size())
    {
        size_t lineEnd = source.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
            lineEnd = source.size();
        std::string_view line = source.substr(lineStart, lineEnd - lineStart);
        size_t variablePos = line.find(variableName);
        if (variablePos != std::string_view::npos)
        {
            size_t inPos = line.rfind(" in ", variablePos);
            if (inPos != std::string_view::npos)
            {
                typeName = trimCopy(line.substr(inPos + 4, variablePos - (inPos + 4)));
                return !typeName.empty();
            }
        }
        lineStart = lineEnd + 1;
    }
    return false;
}

static bool nx1LineContainsPatchedInput(std::string_view line,
    const std::vector<Nx1RawVfetchDecodeElement>& elements)
{
    if (line.find(" in ") == std::string_view::npos)
        return false;
    for (const Nx1RawVfetchDecodeElement& element : elements)
    {
        if (!element.typeName.empty() && line.find(element.variableName) != std::string_view::npos)
            return true;
    }
    return false;
}

static void nx1RemovePatchedInputLines(std::string& source,
    const std::vector<Nx1RawVfetchDecodeElement>& elements)
{
    std::string patched;
    patched.reserve(source.size());
    size_t lineStart = 0;
    while (lineStart < source.size())
    {
        size_t lineEnd = source.find('\n', lineStart);
        const bool hasNewline = lineEnd != std::string::npos;
        if (!hasNewline)
            lineEnd = source.size();
        std::string_view line(source.data() + lineStart, lineEnd - lineStart);
        if (!nx1LineContainsPatchedInput(line, elements))
        {
            patched.append(line);
            if (hasNewline)
                patched.push_back('\n');
        }
        lineStart = lineEnd + (hasNewline ? 1 : 0);
    }
    source = std::move(patched);
}

static const char* nx1ShaderSideVfetchHelpers = R"(
#ifndef __spirv__
ByteAddressBuffer g_Nx1SharedMemory : register(t0, space4);
#ifdef b3
#define NX1_RESTORE_B3_MACRO 1
#undef b3
#endif
cbuffer Nx1FetchConstants : register(b3, space4)
{
    uint4 g_Nx1FetchConstants[48] : packoffset(c0);
};
#ifdef NX1_RESTORE_B3_MACRO
#define b3 (1 << 3)
#undef NX1_RESTORE_B3_MACRO
#endif

uint2 Nx1GetVertexFetchConstant(uint index)
{
    uint4 packedFetch = g_Nx1FetchConstants[index >> 1];
    return (index & 1) != 0 ? packedFetch.zw : packedFetch.xy;
}

uint Nx1EndianSwap32(uint value, uint endian)
{
    if (endian == 1u)
        return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    if (endian == 2u)
    {
        value = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
        return (value << 16) | (value >> 16);
    }
    if (endian == 3u)
        return (value << 16) | (value >> 16);
    return value;
}

uint Nx1SignExtend(uint value, uint bits)
{
    uint shift = 32u - bits;
    return uint(int(value << shift) >> shift);
}

float Nx1UnsignedNormalized(uint value, uint bits)
{
    return float(value) / float((1u << bits) - 1u);
}

float Nx1SignedComponent(uint value, uint bits, bool isInteger)
{
    int signedValue = int(Nx1SignExtend(value, bits));
    if (isInteger)
        return float(signedValue);
    return max(float(signedValue) / float((1u << (bits - 1u)) - 1u), -1.0);
}

float Nx1UnsignedComponent(uint value, uint bits, bool isInteger)
{
    if (isInteger)
        return float(value);
    return Nx1UnsignedNormalized(value, bits);
}

float Nx1Component(uint value, uint bits, bool isSigned, bool isInteger)
{
    return isSigned ? Nx1SignedComponent(value, bits, isInteger)
                    : Nx1UnsignedComponent(value, bits, isInteger);
}

uint4 Nx1LoadVfetchWords(uint vertexId, uint fetchConstant, int offsetWords, uint strideWords)
{
    uint2 fetch = Nx1GetVertexFetchConstant(fetchConstant);
    uint address = (fetch.x & ~3u) + vertexId * strideWords * 4u + uint(offsetWords * 4);
    uint endian = fetch.y & 3u;
    uint4 words;
    words.x = Nx1EndianSwap32(g_Nx1SharedMemory.Load(address + 0u), endian);
    words.y = Nx1EndianSwap32(g_Nx1SharedMemory.Load(address + 4u), endian);
    words.z = Nx1EndianSwap32(g_Nx1SharedMemory.Load(address + 8u), endian);
    words.w = Nx1EndianSwap32(g_Nx1SharedMemory.Load(address + 12u), endian);
    return words;
}

float4 Nx1DecodeVfetchFloat4(uint vertexId, uint fetchConstant, int offsetWords,
    uint strideWords, uint format, bool isSigned, bool isInteger)
{
    uint4 words = Nx1LoadVfetchWords(vertexId, fetchConstant, offsetWords, strideWords);
    if (format == 38u)
        return asfloat(words);
    if (format == 57u)
        return float4(asfloat(words.xyz), 1.0);
    if (format == 37u)
        return float4(asfloat(words.xy), 0.0, 1.0);
    if (format == 36u)
        return float4(asfloat(words.x), 0.0, 0.0, 1.0);
    if (format == 35u)
    {
        if (isSigned)
            return float4(int4(words));
        return float4(words);
    }
    if (format == 34u)
    {
        if (isSigned)
            return float4(float2(int2(words.xy)), 0.0, 1.0);
        return float4(float2(words.xy), 0.0, 1.0);
    }
    if (format == 33u)
    {
        if (isSigned)
            return float4(float(int(words.x)), 0.0, 0.0, 1.0);
        return float4(float(words.x), 0.0, 0.0, 1.0);
    }
    if (format == 32u)
        return float4(f16tof32(words.x & 0xFFFFu), f16tof32(words.x >> 16),
                      f16tof32(words.y & 0xFFFFu), f16tof32(words.y >> 16));
    if (format == 31u)
        return float4(f16tof32(words.x & 0xFFFFu), f16tof32(words.x >> 16), 0.0, 1.0);
    if (format == 26u)
    {
        uint4 v = uint4(words.x & 0xFFFFu, words.x >> 16, words.y & 0xFFFFu, words.y >> 16);
        return float4(Nx1Component(v.x, 16u, isSigned, isInteger),
                      Nx1Component(v.y, 16u, isSigned, isInteger),
                      Nx1Component(v.z, 16u, isSigned, isInteger),
                      Nx1Component(v.w, 16u, isSigned, isInteger));
    }
    if (format == 25u)
    {
        uint2 v = uint2(words.x & 0xFFFFu, words.x >> 16);
        return float4(Nx1Component(v.x, 16u, isSigned, isInteger),
                      Nx1Component(v.y, 16u, isSigned, isInteger), 0.0, 1.0);
    }
    if (format == 7u)
    {
        uint4 v = uint4(words.x & 0x3FFu, (words.x >> 10) & 0x3FFu,
                         (words.x >> 20) & 0x3FFu, words.x >> 30);
        return float4(Nx1Component(v.x, 10u, isSigned, isInteger),
                      Nx1Component(v.y, 10u, isSigned, isInteger),
                      Nx1Component(v.z, 10u, isSigned, isInteger),
                      Nx1Component(v.w, 2u, isSigned, isInteger));
    }
    if (format == 6u)
    {
        uint4 v = uint4(words.x & 0xFFu, (words.x >> 8) & 0xFFu,
                         (words.x >> 16) & 0xFFu, words.x >> 24);
        return float4(Nx1Component(v.x, 8u, isSigned, isInteger),
                      Nx1Component(v.y, 8u, isSigned, isInteger),
                      Nx1Component(v.z, 8u, isSigned, isInteger),
                      Nx1Component(v.w, 8u, isSigned, isInteger));
    }
    return asfloat(words);
}

uint4 Nx1DecodeVfetchUint4(uint vertexId, uint fetchConstant, int offsetWords,
    uint strideWords, uint format, bool isSigned, bool isInteger)
{
    uint4 words = Nx1LoadVfetchWords(vertexId, fetchConstant, offsetWords, strideWords);
    if (format == 7u || format == 6u || format == 16u || format == 17u)
        return uint4(words.x, 0u, 0u, 0u);
    if (format == 26u)
        return uint4(words.x & 0xFFFFu, words.x >> 16, words.y & 0xFFFFu, words.y >> 16);
    if (format == 25u)
        return uint4(words.x & 0xFFFFu, words.x >> 16, 0u, 1u);
    return words;
}
#endif
)";

static bool patchNx1RawVertexFetchHlsl(std::string& source, const char* inputPath)
{
    std::vector<Nx1RawVfetchDecodeElement> elements;
    readNx1RawVertexFetchDecodeMetadata(inputPath, elements);
    if (elements.empty())
        return false;

    size_t patchedElementCount = 0;
    for (Nx1RawVfetchDecodeElement& element : elements)
    {
        if (nx1FindInputType(source, element.variableName, element.typeName))
            ++patchedElementCount;
    }
    if (patchedElementCount == 0)
        return false;

    nx1RemovePatchedInputLines(source, elements);

    size_t shaderMarker = source.find("#ifndef __spirv__\n[shader(\"vertex\")]");
    if (shaderMarker == std::string::npos)
        shaderMarker = source.find("#ifndef __spirv__\r\n[shader(\"vertex\")]");
    if (shaderMarker != std::string::npos)
        source.insert(shaderMarker, nx1ShaderSideVfetchHelpers);

    size_t mainPos = source.find("void main(");
    if (mainPos == std::string::npos)
        return false;
    size_t signatureLineEnd = source.find('\n', mainPos);
    if (signatureLineEnd == std::string::npos)
        return false;
    source.insert(signatureLineEnd + 1, "\tuint nx1VertexId : SV_VertexID,\n");

    size_t bodyPos = source.find('{', signatureLineEnd);
    if (bodyPos == std::string::npos)
        return false;

    std::string declarations = "\n";
    // The guest index buffer is big-endian; D3D12 reads SV_VertexID as
    // little-endian, so swap it back by the per-draw index endianness before
    // using it to address vertices in shared memory (mirrors the translated
    // path's vertex_index_endian swap). Without this the native VS fetches
    // vertices at byte-swapped indices and produces garbage/NaN positions.
    declarations += "\tuint nx1RealVertexId = Nx1EndianSwap32(nx1VertexId, g_VertexIndexEndian);\n";
    for (const Nx1RawVfetchDecodeElement& element : elements)
    {
        if (element.typeName.empty())
            continue;
        declarations += fmt::format("\t{} {} = Nx1DecodeVfetch{}(nx1RealVertexId, {}u, {}, {}u, {}u, {}, {});\n",
            element.typeName, element.variableName, element.typeName == "uint4" ? "Uint4" : "Float4",
            element.fetchConstant, element.offsetWords, element.strideWords, element.format,
            element.isSigned ? "true" : "false", element.isInteger ? "true" : "false");
    }
    source.insert(bodyPos + 1, declarations);
    return true;
}

static PixelShaderOutputs inferNx1RawPixelOutputs(const uint8_t* rawData, size_t rawSize)
{
    PixelShaderOutputs outputs = PixelShaderOutputs(0);
    if (rawSize < 12 || (rawSize & 3) != 0)
        return PIXEL_SHADER_OUTPUT_COLOR0;

    const auto code = reinterpret_cast<const uint32_t*>(rawData);
    const uint32_t instrSize = uint32_t(rawSize);
    uint32_t instrAddress = 0;
    auto controlFlowCode = code;

    while (instrAddress < instrSize)
    {
        union
        {
            ControlFlowInstruction controlFlow[2];
            struct
            {
                uint32_t code0;
                uint32_t code1;
                uint32_t code2;
                uint32_t code3;
            };
        };

        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                break;

            default:
                break;
            }

            if (count == 0)
                continue;

            const size_t instructionDwordOffset = size_t(address) * 3;
            if ((instructionDwordOffset + size_t(count) * 3) * sizeof(uint32_t) > rawSize)
                continue;

            auto instructionCode = code + instructionDwordOffset;
            for (uint32_t i = 0; i < count; ++i)
            {
                if ((sequence & 0x1) == 0)
                {
                    union
                    {
                        AluInstruction alu;
                        struct
                        {
                            uint32_t code0;
                            uint32_t code1;
                            uint32_t code2;
                        };
                    };

                    code0 = instructionCode[0];
                    code1 = instructionCode[1];
                    code2 = instructionCode[2];

                    if (alu.exportData)
                    {
                        switch (ExportRegister(alu.vectorDest))
                        {
                        case ExportRegister::PSColor0:
                            outputs = PixelShaderOutputs(outputs | PIXEL_SHADER_OUTPUT_COLOR0);
                            break;
                        case ExportRegister::PSColor1:
                            outputs = PixelShaderOutputs(outputs | PIXEL_SHADER_OUTPUT_COLOR1);
                            break;
                        case ExportRegister::PSColor2:
                            outputs = PixelShaderOutputs(outputs | PIXEL_SHADER_OUTPUT_COLOR2);
                            break;
                        case ExportRegister::PSColor3:
                            outputs = PixelShaderOutputs(outputs | PIXEL_SHADER_OUTPUT_COLOR3);
                            break;
                        case ExportRegister::PSDepth:
                            outputs = PixelShaderOutputs(outputs | PIXEL_SHADER_OUTPUT_DEPTH);
                            break;
                        default:
                            break;
                        }
                    }
                }

                sequence >>= 2;
                instructionCode += 3;
            }
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    return outputs ? outputs : PIXEL_SHADER_OUTPUT_COLOR0;
}

static std::vector<uint8_t> buildNx1RawShaderContainer(const uint8_t* rawData, size_t rawSize,
    bool isPixelShader, const std::vector<Nx1RawVertexElement>& vertexElements)
{
    std::vector<uint8_t> container;
    if ((rawSize & 3) != 0)
        return container;

    for (size_t i = 0; i < sizeof(ShaderContainer); ++i)
        container.push_back(0);

    const size_t constantTableOffset = container.size();
    auto constantTable = buildNx1RawConstantTable(isPixelShader);
    container.insert(container.end(), constantTable.begin(), constantTable.end());
    alignTo4(container);

    const size_t shaderOffset = container.size();
    appendU32BE(container, 0);                 // physicalOffset
    appendU32BE(container, uint32_t(rawSize)); // size in bytes
    appendU32BE(container, 0);                 // field8
    appendU32BE(container, 0);                 // fieldC
    appendU32BE(container, 0);                 // field10

    if (isPixelShader)
    {
        constexpr uint32_t kInterpolatorCount = 16;
        appendU32BE(container, kInterpolatorCount << 5);
        appendU32BE(container, 0); // field18
        appendU32BE(container, inferNx1RawPixelOutputs(rawData, rawSize));

        for (uint32_t i = 0; i < kInterpolatorCount; ++i)
            appendU32BE(container, makeInterpolator(DeclUsage::TexCoord, i, i));
    }
    else
    {
        constexpr uint32_t kInterpolatorCount = 16;
        appendU32BE(container, kInterpolatorCount << 5);
        appendU32BE(container, 0); // field18
        appendU32BE(container, uint32_t(vertexElements.size()));
        appendU32BE(container, 0); // field20

        for (const auto& vertexElement : vertexElements)
            appendU32BE(container, makeVertexElement(vertexElement.address, vertexElement.usage, vertexElement.usageIndex));

        for (uint32_t i = 0; i < kInterpolatorCount; ++i)
            appendU32BE(container, makeInterpolator(DeclUsage::TexCoord, i, i));
    }

    alignTo4(container);
    const size_t virtualSize = container.size();

    for (size_t i = 0; i < rawSize; i += sizeof(uint32_t))
    {
        uint32_t word;
        std::memcpy(&word, rawData + i, sizeof(word));
        appendU32BE(container, word);
    }

    writeU32BE(container, 0, 0x102A1100 | (isPixelShader ? 0 : 1));
    writeU32BE(container, 4, uint32_t(virtualSize));
    writeU32BE(container, 8, uint32_t(rawSize));
    writeU32BE(container, 12, 0);
    writeU32BE(container, 16, uint32_t(constantTableOffset));
    writeU32BE(container, 20, 0);
    writeU32BE(container, 24, uint32_t(shaderOffset));
    writeU32BE(container, 28, 0);
    writeU32BE(container, 32, 0);

    return container;
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    std::vector<uint8_t> dxil;
    std::vector<uint8_t> spirv;
    uint32_t specConstantsMask = 0;
    std::filesystem::path nx1RawShaderPath;
    bool nx1RawVertexShader = false;

    // Native D3D9 (Shader Model 3.0) variant.
    std::vector<uint8_t> sm3;
    std::vector<uint16_t> sm3ConstantRemap;
    uint32_t sm3Flags = 0;
};

// Nx1Sm3CacheEntry::flags
enum Nx1Sm3Flags : uint32_t
{
    NX1_SM3_PIXEL_SHADER = 1u << 0,
    NX1_SM3_UNCOMPACTED_CONSTANTS = 1u << 1,  ///< upload all 256 registers 1:1
    NX1_SM3_NEEDS_HOST_HALF_PIXEL = 1u << 2,  ///< host must fold the half-pixel bias
};

int main(int argc, char** argv)
{
#ifndef XENOS_RECOMP_INPUT
    if (argc < 4)
    {
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path]");
        return 0;
    }
#endif

    const char* input =
#ifdef XENOS_RECOMP_INPUT 
        XENOS_RECOMP_INPUT
#else
        argv[1]
#endif
    ;

    const char* output =
#ifdef XENOS_RECOMP_OUTPUT 
        XENOS_RECOMP_OUTPUT
#else
        argv[2]
#endif
        ;
    
    const char* includeInput =
#ifdef XENOS_RECOMP_INCLUDE_INPUT
        XENOS_RECOMP_INCLUDE_INPUT
#else
        argv[3]
#endif
        ;

    // Optional: also emit a Shader Model 3.0 cache for the native D3D9 renderer.
    //   XenosRecomp <in> <out> <include> --sm3 <sm3 output path>
    const char* sm3Output = nullptr;
    // Optional: --sm3-hlsl <dir> writes the SM3 HLSL each shader is compiled from, so a
    // misbehaving translated shader can be read (and diffed against the DXIL lowering).
    const char* sm3HlslDir = nullptr;
    for (int i = 4; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--sm3") == 0)
            sm3Output = argv[i + 1];
        else if (std::strcmp(argv[i], "--sm3-hlsl") == 0)
            sm3HlslDir = argv[i + 1];
    }
    std::atomic<uint32_t> sm3Failures = 0;

    size_t includeSize = 0;
    auto includeData = readAllBytes(includeInput, includeSize);
    std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

    if (std::filesystem::is_directory(input))
    {
        const bool verboseShaderCacheLog = nx1VerboseShaderCacheLog();
        if (verboseShaderCacheLog)
            fmt::println(stderr, "Scanning shader directory: {}", input);
        std::vector<std::unique_ptr<uint8_t[]>> files;
        std::map<XXH64_hash_t, RecompiledShader> shaders;
        size_t rawShaderCount = 0;

        for (auto& file : std::filesystem::recursive_directory_iterator(input))
        {
            if (std::filesystem::is_directory(file))
            {
                continue;
            }
            
            size_t fileSize = 0;
            auto fileData = readAllBytes(file.path().string().c_str(), fileSize);
            bool foundAny = false;

            bool isRawPixelShader = false;
            uint64_t rawUcodeHash = 0;
            if (tryGetNx1RawShaderInfo(file.path(), isRawPixelShader, rawUcodeHash))
            {
                std::vector<Nx1RawVertexElement> vertexElements;
                if (!isRawPixelShader)
                    readNx1RawVertexFetchMetadata(file.path().string().c_str(), vertexElements);

                std::vector<uint8_t> rawContainer =
                    buildNx1RawShaderContainer(fileData.get(), fileSize, isRawPixelShader, vertexElements);
                if (!rawContainer.empty())
                {
                    auto shader = shaders.try_emplace(rawUcodeHash);
                    if (shader.second)
                    {
                        auto ownedContainer = std::make_unique<uint8_t[]>(rawContainer.size());
                        std::memcpy(ownedContainer.get(), rawContainer.data(), rawContainer.size());
                        shader.first->second.data = ownedContainer.get();
                        shader.first->second.nx1RawShaderPath = file.path();
                        shader.first->second.nx1RawVertexShader = !isRawPixelShader;
                        files.emplace_back(std::move(ownedContainer));
                        ++rawShaderCount;
                    }
                }
                continue;
            }

            for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
            {
                auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                size_t dataSize = shaderContainer->virtualSize + shaderContainer->physicalSize;

                if ((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100 &&
                    dataSize <= (fileSize - i) &&
                    shaderContainer->field1C == 0 &&
                    shaderContainer->field20 == 0)
                {
                    XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                    auto shader = shaders.try_emplace(hash);
                    if (shader.second)
                    {
                        shader.first->second.data = fileData.get() + i;
                        foundAny = true;
                    }

                    i += dataSize;
                }
                else
                {
                    i += sizeof(uint32_t);
                }
            }

            if (foundAny)
                files.emplace_back(std::move(fileData));
        }

        if (rawShaderCount != 0)
            fmt::println("Found {} raw NX1 shader dump(s).", rawShaderCount);
        if (verboseShaderCacheLog)
            fmt::println(stderr, "Found {} shader(s), {} raw NX1 dump(s).", shaders.size(), rawShaderCount);

        const bool nx1RawDirectoryMode = rawShaderCount != 0;
        std::atomic<uint32_t> progress = 0;
        std::atomic<uint32_t> compileFailures = 0;

        std::for_each(std::execution::seq, shaders.begin(), shaders.end(), [&](auto& hashShaderPair)
            {
                auto& shader = hashShaderPair.second;

                ShaderRecompiler recompiler;
                recompiler.recompile(shader.data, include);

                shader.specConstantsMask = recompiler.specConstantsMask;

                // The SM3 variant is built from the *unpatched* HLSL: the
                // SV_VertexID + vfetch patch below is a D3D12 workaround, and
                // D3D9 has a real input assembler.
                if (sm3Output != nullptr)
                {
                    Sm3Shader sm3;
                    std::string sm3Error;
                    if (!transformToSm3(recompiler.out, recompiler.isPixelShader,
                                        recompiler.specConstantsMask, sm3, sm3Error))
                    {
                        fmt::println(stderr, "SM3 transform failed for shader 0x{:016X}: {}",
                            uint64_t(hashShaderPair.first), sm3Error);
                        ++sm3Failures;
                    }
                    else
                    {
                        if (sm3HlslDir != nullptr)
                        {
                            std::filesystem::create_directories(sm3HlslDir);
                            const std::string path =
                                fmt::format("{}/shader_{:016X}.sm3.{}.hlsl", sm3HlslDir,
                                            uint64_t(hashShaderPair.first),
                                            recompiler.isPixelShader ? "frag" : "vert");
                            std::ofstream f(path);
                            f << sm3.hlsl;
                        }
                        shader.sm3 = compileSm3(sm3.hlsl, recompiler.isPixelShader, sm3Error);
                        if (shader.sm3.empty())
                        {
                            fmt::println(stderr, "SM3 compile failed for shader 0x{:016X}: {}",
                                uint64_t(hashShaderPair.first), sm3Error);
                            ++sm3Failures;
                        }
                        else
                        {
                            shader.sm3ConstantRemap = std::move(sm3.constantRemap);
                            shader.sm3Flags =
                                (recompiler.isPixelShader ? NX1_SM3_PIXEL_SHADER : 0u) |
                                (sm3.uncompactedConstants ? NX1_SM3_UNCOMPACTED_CONSTANTS : 0u) |
                                (sm3.needsHostHalfPixel ? NX1_SM3_NEEDS_HOST_HALF_PIXEL : 0u);
                        }
                    }
                }

                std::string hlsl = recompiler.out;
                if (shader.nx1RawVertexShader)
                    patchNx1RawVertexFetchHlsl(hlsl, shader.nx1RawShaderPath.string().c_str());
                if (verboseShaderCacheLog)
                {
                    fmt::println(stderr, "Compiling shader 0x{:016X} ({})",
                        uint64_t(hashShaderPair.first), shader.nx1RawShaderPath.string());
                }

                DxcCompiler dxcCompiler;

#ifdef XENOS_RECOMP_DXIL
                IDxcBlob* dxilBlob = nullptr;
                {
                    std::lock_guard lock(nx1DxcCompileMutex());
                    dxilBlob = dxcCompiler.compile(hlsl, recompiler.isPixelShader,
                        false, false);
                }
                if (verboseShaderCacheLog)
                {
                    fmt::println(stderr, "Compiled shader 0x{:016X} ({})",
                        uint64_t(hashShaderPair.first), shader.nx1RawShaderPath.string());
                }
                if (dxilBlob == nullptr)
                {
                    const auto failedHlslPath = std::filesystem::path(output).parent_path() /
                        fmt::format("xenosrecomp_failed_{:016X}.hlsl", uint64_t(hashShaderPair.first));
                    writeAllBytes(failedHlslPath.string().c_str(), hlsl.data(), hlsl.size());
                    fmt::println(stderr, "DXIL compile failed for shader 0x{:016X} ({})",
                        uint64_t(hashShaderPair.first), shader.nx1RawShaderPath.string());
                    ++compileFailures;
                    return;
                }
                if (*(reinterpret_cast<uint32_t *>(dxilBlob->GetBufferPointer()) + 1) == 0)
                {
                    fmt::println(stderr, "DXIL compile produced invalid blob for shader 0x{:016X} ({})",
                        uint64_t(hashShaderPair.first), shader.nx1RawShaderPath.string());
                    dxilBlob->Release();
                    ++compileFailures;
                    return;
                }
                shader.dxil.assign(reinterpret_cast<uint8_t *>(dxilBlob->GetBufferPointer()),
                    reinterpret_cast<uint8_t *>(dxilBlob->GetBufferPointer()) + dxilBlob->GetBufferSize());
                dxilBlob->Release();
#endif

                if (!nx1RawDirectoryMode)
                {
                    IDxcBlob* spirv = nullptr;
                    {
                        std::lock_guard lock(nx1DxcCompileMutex());
                        spirv = dxcCompiler.compile(hlsl, recompiler.isPixelShader, false, true);
                    }
                    if (spirv == nullptr)
                    {
                        fmt::println(stderr, "SPIR-V compile failed for shader 0x{:016X}",
                            uint64_t(hashShaderPair.first));
                        ++compileFailures;
                        return;
                    }

                    bool result = smolv::Encode(spirv->GetBufferPointer(), spirv->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo);
                    if (!result)
                    {
                        fmt::println(stderr, "smolv encode failed for shader 0x{:016X}",
                            uint64_t(hashShaderPair.first));
                        ++compileFailures;
                        spirv->Release();
                        return;
                    }

                    spirv->Release();
                }

                size_t currentProgress = ++progress;
                if ((currentProgress % 10) == 0 || (currentProgress == shaders.size() - 1))
                    fmt::println("Recompiling shaders... {}%", currentProgress / float(shaders.size()) * 100.0f);
            });

        if (compileFailures != 0)
        {
            // Don't abort the whole cache for a handful of shaders that DXC
            // can't compile - emit them with empty DXIL (the runtime treats a
            // zero-size entry as a cache miss and falls back to the translated
            // shader for that draw). This keeps the cache usable instead of
            // failing the entire build for one bad shader.
            fmt::println("Warning: {} shader(s) failed to compile; emitting them as cache misses.",
                compileFailures.load());
        }

        fmt::println("Creating shader cache...");

        StringBuffer f;
        f.println("#include \"shader_cache.h\"");
        f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

        std::vector<uint8_t> dxil;
        std::vector<uint8_t> spirv;

        for (auto& [hash, shader] : shaders)
        {
            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {} }},",
                hash, dxil.size(), shader.dxil.size(), spirv.size(), shader.spirv.size(), shader.specConstantsMask);

            if (!shader.dxil.empty())
            {
                dxil.insert(dxil.end(), shader.dxil.begin(), shader.dxil.end());
            }
            
            spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());
        }

        f.println("}};");

        fmt::println("Compressing DXIL cache...");

        int level = ZSTD_maxCLevel();

#ifdef XENOS_RECOMP_DXIL
        std::vector<uint8_t> dxilCompressed(ZSTD_compressBound(dxil.size()));
        dxilCompressed.resize(ZSTD_compress(dxilCompressed.data(), dxilCompressed.size(), dxil.data(), dxil.size(), level));

        f.print("const uint8_t g_compressedDxilCache[] = {{");

        for (auto data : dxilCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_dxilCacheCompressedSize = {};", dxilCompressed.size());
        f.println("const size_t g_dxilCacheDecompressedSize = {};", dxil.size());
#endif

        fmt::println("Compressing SPIRV cache...");

        std::vector<uint8_t> spirvCompressed;
        if (!spirv.empty())
        {
            spirvCompressed.resize(ZSTD_compressBound(spirv.size()));
            spirvCompressed.resize(ZSTD_compress(spirvCompressed.data(), spirvCompressed.size(), spirv.data(), spirv.size(), level));
        }

        f.print("const uint8_t g_compressedSpirvCache[] = {{");

        for (auto data : spirvCompressed)
            f.print("{},", data);

        f.println("}};");

        f.println("const size_t g_spirvCacheCompressedSize = {};", spirvCompressed.size());
        f.println("const size_t g_spirvCacheDecompressedSize = {};", spirv.size());
        f.println("const size_t g_shaderCacheEntryCount = {};", shaders.size());

        writeAllBytes(output, f.out.data(), f.out.size());

        if (sm3Output != nullptr)
        {
            if (sm3Failures != 0)
            {
                // A zero-size entry is a cache miss: that draw falls back to the
                // translated path rather than failing the whole build.
                fmt::println("Warning: {} shader(s) failed SM3 lowering; emitting them as cache misses.",
                    sm3Failures.load());
            }

            fmt::println("Creating SM3 shader cache...");

            StringBuffer s;
            s.println("#include \"nx1_sm3_shader_cache.h\"");
            s.println("Nx1Sm3CacheEntry g_nx1Sm3CacheEntries[] = {{");

            std::vector<uint8_t> bytecode;
            std::vector<uint16_t> remap;
            size_t emitted = 0;

            for (auto& [hash, shader] : shaders)
            {
                s.println("\t{{ 0x{:X}, {}, {}, {}, {}, {} }},", hash, bytecode.size(),
                    shader.sm3.size(), remap.size(), shader.sm3ConstantRemap.size(), shader.sm3Flags);

                bytecode.insert(bytecode.end(), shader.sm3.begin(), shader.sm3.end());
                remap.insert(remap.end(), shader.sm3ConstantRemap.begin(), shader.sm3ConstantRemap.end());
                if (!shader.sm3.empty())
                    ++emitted;
            }

            s.println("}};");

            s.print("const uint16_t g_nx1Sm3ConstantRemap[] = {{");
            for (uint16_t v : remap)
                s.print("{},", v);
            s.println("0}};");

            std::vector<uint8_t> sm3Compressed;
            if (!bytecode.empty())
            {
                sm3Compressed.resize(ZSTD_compressBound(bytecode.size()));
                sm3Compressed.resize(ZSTD_compress(sm3Compressed.data(), sm3Compressed.size(),
                    bytecode.data(), bytecode.size(), level));
            }

            s.print("const uint8_t g_compressedNx1Sm3Cache[] = {{");
            for (auto data : sm3Compressed)
                s.print("{},", data);
            s.println("}};");

            s.println("const size_t g_nx1Sm3CacheCompressedSize = {};", sm3Compressed.size());
            s.println("const size_t g_nx1Sm3CacheDecompressedSize = {};", bytecode.size());
            s.println("const size_t g_nx1Sm3CacheEntryCount = {};", shaders.size());

            writeAllBytes(sm3Output, s.out.data(), s.out.size());
            fmt::println("SM3 cache: {}/{} shaders, {} KiB compressed.",
                emitted, shaders.size(), sm3Compressed.size() / 1024);
        }
    }
    else
    {
        ShaderRecompiler recompiler;
        size_t fileSize;
        auto inputData = readAllBytes(input, fileSize);
        std::string_view inputPath(input);
        std::vector<uint8_t> nx1RawContainer;
        if (endsWith(inputPath, ".ucode.bin.frag") || endsWith(inputPath, ".ucode.bin.vert"))
        {
            std::vector<Nx1RawVertexElement> vertexElements;
            if (endsWith(inputPath, ".ucode.bin.vert"))
                readNx1RawVertexFetchMetadata(input, vertexElements);

            nx1RawContainer = buildNx1RawShaderContainer(inputData.get(), fileSize,
                endsWith(inputPath, ".ucode.bin.frag"), vertexElements);
            if (nx1RawContainer.empty())
            {
                printf("Raw NX1 ucode input size must be a multiple of 4 bytes.");
                return 1;
            }
            recompiler.recompile(nx1RawContainer.data(), include);
            if (endsWith(inputPath, ".ucode.bin.vert"))
                patchNx1RawVertexFetchHlsl(recompiler.out, input);
        }
        else
        {
            recompiler.recompile(inputData.get(), include);
        }
        if (std::getenv("NX1_XENOSRECOMP_DXIL_SINGLE") != nullptr)
        {
            DxcCompiler dxcCompiler;
            IDxcBlob* dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader,
                false, false);
            if (dxil == nullptr)
            {
                fprintf(stderr, "DXIL compile failed for %s\n", input);
                return 2;
            }
            writeAllBytes(output, dxil->GetBufferPointer(), dxil->GetBufferSize());
            dxil->Release();
            return 0;
        }
        writeAllBytes(output, recompiler.out.data(), recompiler.out.size());
    }

    return 0;
}
