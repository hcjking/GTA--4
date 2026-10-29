// Applies the native renderer's SPIR-V rewrites to the extracted shaders, to
// see how close they get to WebGPU: constant banks and shared constants from
// buffer-device-address loads into uniform buffers, and the descriptor heaps
// onto the merged layout. Reports what still needs buffer pointers.
// Usage: rewrite_spirv rewrite|strip <index.tsv dir> <out dir>
//   rewrite: the constant and descriptor passes (run spirv-opt's dead-code
//            elimination next, so the now-unused pointer casts go away);
//   strip:   drops the buffer-pointer capability from shaders that no longer
//            use pointers.
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "native_spirv_descriptor_remap.h"
#include "native_spirv_ubo_constants.h"
#include "native_spirv_ubo_shared.h"

using namespace rex::graphics::gta4_native;

constexpr uint32_t kOpCapability = 17, kOpConvertUToPtr = 120;
constexpr uint32_t kCapabilityPhysicalStorageBufferAddresses = 5347;

// What naga can't take as the shaders are written:
// - gl_SampleMask declared as uint[1] (naga wants a plain uint): the variable
//   becomes a uint and the access chains into element 0 become the variable;
// - OpIsNan, which WGSL has no function for: x != x (unordered).
std::vector<uint32_t> FixForNaga(const std::vector<uint32_t>& words) {
  constexpr uint32_t kOpDecorate = 71, kOpVariable = 59, kOpAccessChain = 65, kOpLoad = 61,
                     kOpStore = 62, kOpIsNan = 156, kOpFUnordNotEqual = 183, kBuiltIn = 11,
                     kSampleMask = 20;
  uint32_t mask_var = 0;
  for (size_t i = 5; i < words.size();) {
    const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
    if (!length) break;
    if (opcode == kOpDecorate && length >= 4 && words[i + 2] == kBuiltIn &&
        words[i + 3] == kSampleMask) {
      mask_var = words[i + 1];
    }
    i += length;
  }
  // The Output uint pointer type the access chains produce.
  uint32_t uint_out_ptr = 0;
  std::set<uint32_t> chains;
  for (size_t i = 5; mask_var && i < words.size();) {
    const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
    if (!length) break;
    if (opcode == kOpAccessChain && length == 5 && words[i + 3] == mask_var) {
      uint_out_ptr = words[i + 1];
      chains.insert(words[i + 2]);
    }
    i += length;
  }
  std::vector<uint32_t> out(words.begin(), words.begin() + 5);
  bool pointer_type_emitted = false;
  size_t hoisted = 0;
  for (size_t i = 5; i < words.size();) {
    const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
    if (!length || i + length > words.size()) break;
    std::vector<uint32_t> inst(words.begin() + i, words.begin() + i + length);
    i += length;
    if (uint_out_ptr) {
      // (that pointer type ahead of the variable when it is declared later,
      // as glslang's output has it)
      if (opcode == kOpVariable && inst[2] == mask_var && !pointer_type_emitted) {
        for (size_t k = 5; k < words.size();) {
          const uint32_t op = words[k] & 0xFFFF, n = words[k] >> 16;
          if (!n) break;
          if (op == 32 /* OpTypePointer */ && words[k + 1] == uint_out_ptr) {
            if (k >= i) {
              out.insert(out.end(), words.begin() + k, words.begin() + k + n);
              hoisted = k;
            }
            break;
          }
          k += n;
        }
        pointer_type_emitted = true;
      }
      if (hoisted && i - length == hoisted) continue;
      if (opcode == kOpVariable && inst[2] == mask_var) inst[1] = uint_out_ptr;
      if (opcode == kOpAccessChain && chains.count(inst[2])) continue;
      if (opcode == kOpStore && chains.count(inst[1])) inst[1] = mask_var;
      if (opcode == kOpLoad && chains.count(inst[3])) inst[3] = mask_var;
    }
    if (opcode == kOpIsNan && length == 4) {
      inst = {(5u << 16) | kOpFUnordNotEqual, inst[1], inst[2], inst[3], inst[3]};
    }
    out.insert(out.end(), inst.begin(), inst.end());
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  const bool strip = std::string(argv[1]) == "strip";
  const std::filesystem::path in = argv[2], out = argv[3];
  std::filesystem::create_directories(out);
  std::ifstream index(in / "index.tsv");
  std::string line;
  size_t shaders = 0, fully_rewritten = 0, total_loads = 0;
  while (std::getline(index, line)) {
    std::istringstream fields(line);
    std::string file, name;
    std::getline(fields, file, '\t');
    std::getline(fields, name, '\t');
    std::ifstream source(in / file, std::ios::binary | std::ios::ate);
    std::vector<uint32_t> words(size_t(source.tellg()) / 4);
    source.seekg(0);
    source.read(reinterpret_cast<char*>(words.data()), words.size() * 4);
    // Stage from the entry point (OpEntryPoint's execution model, 0 = vertex):
    // names from .fxc containers don't say (*_vsN.bin / .vert do).
    bool vertex = name.find("_vs") != std::string::npos || name.find(".vert") != std::string::npos;
    for (size_t i = 5; i < words.size();) {
      const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
      if (!length) break;
      if (opcode == 15 /* OpEntryPoint */ && length >= 2) {
        vertex = words[i + 1] == 0;
        break;
      }
      i += length;
    }
    if (!strip) {
    // (256 vertex registers, 224 pixel ones - the native kPixelConstantsSize,
    // 0xE00: relative reads past the last pixel register clamp to it, as there)
    if (auto r = RewriteSpirvConstantBankToUbo(words, vertex ? 0 : 1, vertex ? 256 : 224, 6,
                                               vertex ? 0 : 1)) {
      total_loads += r->rewritten_loads;
      if (r->rewritten_loads) words = std::move(r->words);
    }
    if (auto r = RewriteSpirvSharedConstantsToUbo(words, 2, 0x500 / 16, 6, 2)) {
      total_loads += r->rewritten_loads;
      if (r->rewritten_loads) words = std::move(r->words);
    }
    RemapSpirvDrawDescriptorSets(words);
    }
    // Count what is left that needs buffer pointers.
    std::set<uint32_t> psb_pointer_types;
    for (size_t i = 5; i < words.size();) {
      const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
      if (!length) break;
      if (opcode == 32 /* OpTypePointer */ && length >= 4 && words[i + 2] == 5349) {
        psb_pointer_types.insert(words[i + 1]);
      }
      i += length;
    }
    size_t pointer_conversions = 0;
    for (size_t i = 5; i < words.size();) {
      const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
      if (!length) break;
      // A pointer made from an address: OpConvertUToPtr, or OpBitcast of a
      // 64-bit value to a physical-storage-buffer pointer type.
      if (opcode == kOpConvertUToPtr) pointer_conversions++;
      if (opcode == 124 /* OpBitcast */ && length >= 4 && psb_pointer_types.count(words[i + 1])) {
        pointer_conversions++;
      }
      i += length;
    }
    if (strip && pointer_conversions) {
      // Buffer-pointer reads left over (shader overrides' FusionFix LUT tone
      // map, behind a shared-constant enable this renderer leaves 0, and
      // their conditional-rendering survey): each load or atomic through such
      // a pointer reads 0 instead, stores through one are dropped, and the
      // pointers go, so the module needs no buffer addresses.
      std::set<uint32_t> pointers;
      std::map<uint32_t, uint32_t> nulls;  // result type -> OpConstantNull id
      uint32_t bound = words[3];
      for (size_t i = 5; i < words.size();) {
        const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
        if (!length) break;
        if ((opcode == kOpConvertUToPtr || opcode == 124) && length >= 4 && psb_pointer_types.count(words[i + 1]))
          pointers.insert(words[i + 2]);
        // (members of a buffer-reference block reached through one are pointers too)
        if ((opcode == 65 /* OpAccessChain */ || opcode == 67 /* OpPtrAccessChain */) && length >= 4 &&
            pointers.count(words[i + 3]))
          pointers.insert(words[i + 2]);
        if ((opcode == 61 /* OpLoad */ || (opcode >= 227 && opcode <= 242) /* atomics */) && length >= 4 &&
            pointers.count(words[i + 3]) && !nulls.count(words[i + 1]))
          nulls[words[i + 1]] = bound++;
        i += length;
      }
      std::vector<uint32_t> kept(words.begin(), words.begin() + 5);
      bool constants_placed = false;
      for (size_t i = 5; i < words.size();) {
        const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
        if (!length || i + length > words.size()) break;
        if (opcode == 54 /* OpFunction */ && !constants_placed) {
          for (const auto& [type, id] : nulls) kept.insert(kept.end(), {(3u << 16) | 46 /* OpConstantNull */, type, id});
          constants_placed = true;
        }
        const bool through = length >= 4 && pointers.count(words[i + 3]);
        if ((opcode == kOpConvertUToPtr || opcode == 124 || opcode == 65 || opcode == 67) && length >= 4 &&
            pointers.count(words[i + 2])) {
          // (the pointer itself)
        } else if (opcode == 39 /* OpTypeForwardPointer */ && length >= 3 && words[i + 2] == 5349) {
        } else if (opcode == 32 /* OpTypePointer */ && length >= 4 && words[i + 2] == 5349) {
          // (a buffer-pointer type, now unused)
        } else if ((opcode == 61 || (opcode >= 227 && opcode <= 242)) && through) {
          kept.insert(kept.end(), {(4u << 16) | 83 /* OpCopyObject */, words[i + 1], words[i + 2], nulls[words[i + 1]]});
        } else if (opcode == 62 /* OpStore */ && length >= 3 && pointers.count(words[i + 1])) {
          // (a store through one)
        } else if ((opcode == 228 /* OpAtomicStore */) && length >= 2 && pointers.count(words[i + 1])) {
        } else {
          kept.insert(kept.end(), words.begin() + i, words.begin() + i + length);
        }
        i += length;
      }
      kept[3] = bound;
      words = std::move(kept);
      std::printf("  %s: %zu buffer-pointer reads now read 0\n", file.c_str(), nulls.size());
      pointer_conversions = 0;
    }
    if (!pointer_conversions) fully_rewritten++;
    if (strip && !pointer_conversions) {
      // Nothing needs buffer pointers any more: drop the capability, its
      // extension, and the 64-bit physical addressing model.
      std::vector<uint32_t> kept(words.begin(), words.begin() + 5);
      for (size_t i = 5; i < words.size();) {
        const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
        if (!length || i + length > words.size()) break;
        bool drop = false;
        if (opcode == kOpCapability && words[i + 1] == kCapabilityPhysicalStorageBufferAddresses) {
          drop = true;
        } else if (opcode == 10 /* OpExtension */) {
          const char* text = reinterpret_cast<const char*>(&words[i + 1]);
          drop = std::string(text) == "SPV_KHR_physical_storage_buffer";
        }
        if (!drop) {
          kept.insert(kept.end(), words.begin() + i, words.begin() + i + length);
          if (opcode == 14 /* OpMemoryModel */ && kept[kept.size() - length + 1] == 5348) {
            kept[kept.size() - length + 1] = 0;  // Logical
          }
        }
        i += length;
      }
      words = std::move(kept);
    }
    if (strip) words = FixForNaga(words);
    std::ofstream(out / file, std::ios::binary)
        .write(reinterpret_cast<const char*>(words.data()), words.size() * 4);
    shaders++;
  }
  std::printf("%zu shaders, %zu loads rewritten, %zu with no buffer-pointer loads left\n", shaders,
              total_loads, fully_rewritten);
  return 0;
}
