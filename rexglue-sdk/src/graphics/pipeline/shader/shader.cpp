/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cinttypes>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/graphics/format/ucode.h>
#include <rex/logging/macros.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/string.h>

namespace rex::graphics {
using namespace ucode;

namespace {

struct Nx1RawVertexUsage {
  const char* name;
  uint32_t usage;
  uint32_t usage_index;
};

Nx1RawVertexUsage GetNx1RawVertexUsage(size_t attribute_ordinal,
                                       xenos::VertexFormat format,
                                       uint32_t& packed_vector_count,
                                       uint32_t& texcoord_count,
                                       uint32_t& color_count) {
  // XenosRecomp's raw NX1 path doesn't receive original D3D declaration
  // semantics from ReXGlue, so generate a stable declaration from the runtime
  // fetch metadata. Keep POSITION0 for the first fetch, map packed 10/11-bit
  // vectors to the conventional normal basis, and use TEXCOORD/COLOR for the
  // remaining generic attributes.
  if (attribute_ordinal == 0) {
    return {"Position", 0, 0};
  }

  switch (format) {
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
      switch (packed_vector_count++) {
        case 0:
          return {"Normal", 3, 0};
        case 1:
          return {"Tangent", 6, 0};
        case 2:
          return {"Binormal", 7, 0};
        default:
          break;
      }
      break;
    case xenos::VertexFormat::k_8_8_8_8:
      return {"Color", 10, color_count++};
    default:
      break;
  }

  return {"TexCoord", 5, texcoord_count++};
}

void WriteNx1RawVertexFetchMetadata(const Shader& shader,
                                    const std::filesystem::path& raw_ucode_path) {
  if (shader.type() != xenos::ShaderType::kVertex || !shader.is_ucode_analyzed()) {
    return;
  }

  std::filesystem::path metadata_path = raw_ucode_path;
  metadata_path += ".nx1_vfetch";
  FILE* metadata_file = filesystem::OpenFile(metadata_path, "w");
  if (!metadata_file) {
    return;
  }

  auto write_line = [&](const std::string& line) {
    fwrite(line.data(), sizeof(char), line.size(), metadata_file);
  };

  write_line("# NX1 ReXGlue raw vertex fetch metadata v1\n");
  write_line(fmt::format("shader_hash {:016X}\n", shader.ucode_data_hash()));
  write_line(
      "# vfetch address usage usage_index fetch_constant binding_index offset_words "
      "stride_words format used_mask result_register signed integer rounded mini\n");

  size_t attribute_ordinal = 0;
  uint32_t packed_vector_count = 0;
  uint32_t texcoord_count = 0;
  uint32_t color_count = 0;
  for (const Shader::VertexBinding& binding : shader.vertex_bindings()) {
    for (const Shader::VertexBinding::Attribute& attribute : binding.attributes) {
      const ParsedVertexFetchInstruction& fetch = attribute.fetch_instr;
      const Nx1RawVertexUsage usage = GetNx1RawVertexUsage(
          attribute_ordinal++, fetch.attributes.data_format, packed_vector_count, texcoord_count,
          color_count);
      write_line(fmt::format(
          "vfetch {} {} {} {} {} {} {} {} {} {} {} {} {} {}\n",
          attribute.instruction_address, usage.name, usage.usage_index, binding.fetch_constant,
          binding.binding_index, fetch.attributes.offset, binding.stride_words,
          uint32_t(fetch.attributes.data_format), fetch.result.GetUsedResultComponents(),
          fetch.result.storage_index, fetch.attributes.is_signed ? 1 : 0,
          fetch.attributes.is_integer ? 1 : 0, fetch.attributes.is_index_rounded ? 1 : 0,
          fetch.is_mini_fetch ? 1 : 0));
    }
  }

  fclose(metadata_file);
}

}  // namespace

Shader::Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
               const uint32_t* ucode_dwords, size_t ucode_dword_count,
               std::endian ucode_source_endian)
    : shader_type_(shader_type), ucode_data_hash_(ucode_data_hash) {
  // We keep ucode data in host native format so it's easier to work with.
  ucode_data_.resize(ucode_dword_count);
  if (std::endian::native != ucode_source_endian) {
    memory::copy_and_swap(ucode_data_.data(), ucode_dwords, ucode_dword_count);
  } else {
    std::memcpy(ucode_data_.data(), ucode_dwords, sizeof(uint32_t) * ucode_dword_count);
  }
}

Shader::~Shader() {
  for (auto it : translations_) {
    delete it.second;
  }
}

std::string Shader::Translation::GetTranslatedBinaryString() const {
  std::string result;
  result.resize(translated_binary_.size());
  std::memcpy(const_cast<char*>(result.data()), translated_binary_.data(),
              translated_binary_.size());
  return result;
}

std::pair<std::filesystem::path, std::filesystem::path> Shader::Translation::Dump(
    const std::filesystem::path& base_path, const char* path_prefix) const {
  if (!is_valid()) {
    return std::make_pair(std::filesystem::path(), std::filesystem::path());
  }

  std::filesystem::path path = base_path;
  // Ensure target path exists. Guard against an unusable dump path (see DumpUcode).
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    try {
      target_path = std::filesystem::absolute(target_path);
      std::filesystem::create_directories(target_path);
    } catch (const std::exception& e) {
      REXGPU_ERROR("dump path '{}' is unusable ({}); skipping shader translation dump",
                   base_path.string(), e.what());
      return std::make_pair(std::filesystem::path(), std::filesystem::path());
    }
  }

  const char* type_extension = shader().type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / fmt::format("shader_{:016X}_{:016X}.{}.bin.{}", shader().ucode_data_hash(),
                                modification(), path_prefix, type_extension);
  FILE* binary_file = filesystem::OpenFile(binary_path, "wb");
  if (binary_file) {
    fwrite(translated_binary_.data(), sizeof(*translated_binary_.data()), translated_binary_.size(),
           binary_file);
    fclose(binary_file);
  }

  std::filesystem::path disasm_path;
  if (!host_disassembly_.empty()) {
    disasm_path =
        target_path / fmt::format("shader_{:016X}_{:016X}.{}.{}", shader().ucode_data_hash(),
                                  modification(), path_prefix, type_extension);
    FILE* disasm_file = filesystem::OpenFile(disasm_path, "w");
    if (disasm_file) {
      fwrite(host_disassembly_.data(), sizeof(*host_disassembly_.data()), host_disassembly_.size(),
             disasm_file);
      fclose(disasm_file);
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

Shader::Translation* Shader::GetOrCreateTranslation(uint64_t modification, bool* is_new) {
  auto it = translations_.find(modification);
  if (it != translations_.end()) {
    if (is_new) {
      *is_new = false;
    }
    return it->second;
  }
  Translation* translation = CreateTranslationInstance(modification);
  translations_.emplace(modification, translation);
  if (is_new) {
    *is_new = true;
  }
  return translation;
}

void Shader::DestroyTranslation(uint64_t modification) {
  auto it = translations_.find(modification);
  if (it == translations_.end()) {
    return;
  }
  delete it->second;
  translations_.erase(it);
}

std::pair<std::filesystem::path, std::filesystem::path> Shader::DumpUcode(
    const std::filesystem::path& base_path) const {
  // Ensure target path exists. A bad dump_shaders path (e.g. one mangled by TOML
  // backslash escaping into a string with embedded newlines/tabs) makes these throw
  // -- and it happens deep inside shader-storage init, so an uncaught throw kills the
  // process with no logged reason. Fail the dump loudly instead of the whole run.
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    try {
      target_path = std::filesystem::absolute(target_path);
      std::filesystem::create_directories(target_path);
    } catch (const std::exception& e) {
      REXGPU_ERROR("dump_shaders path '{}' is unusable ({}); skipping shader dump",
                   base_path.string(), e.what());
      return {};
    }
  }

  const char* type_extension = type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / fmt::format("shader_{:016X}.ucode.bin.{}", ucode_data_hash(), type_extension);
  FILE* binary_file = filesystem::OpenFile(binary_path, "wb");
  if (binary_file) {
    fwrite(ucode_data().data(), sizeof(*ucode_data().data()), ucode_data().size(), binary_file);
    fclose(binary_file);
  }
  WriteNx1RawVertexFetchMetadata(*this, binary_path);

  std::filesystem::path disasm_path;
  if (is_ucode_analyzed()) {
    disasm_path =
        target_path / fmt::format("shader_{:016X}.ucode.{}", ucode_data_hash(), type_extension);
    FILE* disasm_file = filesystem::OpenFile(disasm_path, "w");
    if (disasm_file) {
      fwrite(ucode_disassembly().data(), sizeof(*ucode_disassembly().data()),
             ucode_disassembly().size(), disasm_file);
      fclose(disasm_file);
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

Shader::Translation* Shader::CreateTranslationInstance(uint64_t modification) {
  // Default implementation for simple cases like ucode disassembly.
  return new Translation(*this, modification);
}

}  // namespace rex::graphics
