// Unpacks LibertyRecompLib's embedded SPIR-V shader cache (each shader stored
// as SMOL-V): one .spv file per
// shader (named by its hash and the original file name), plus index.tsv.
// Build: clang++ -std=c++20 -O1 extract_spirv.cpp <repo>/LibertyRecompLib/shader/shader_cache.cpp
//        -I<repo>/LibertyRecompLib/shader -I/opt/homebrew/include -L/opt/homebrew/lib -lzstd
// Usage: extract_spirv <out dir>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zstd.h>

#include "smolv.h"

#include "shader_cache.h"

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::filesystem::path out = argv[1];
  std::filesystem::create_directories(out);
  std::vector<uint8_t> spirv(g_spirvCacheDecompressedSize);
  const size_t got = ZSTD_decompress(spirv.data(), spirv.size(), g_compressedSpirvCache,
                                     g_spirvCacheCompressedSize);
  if (ZSTD_isError(got)) {
    std::fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(got));
    return 1;
  }
  std::ofstream index(out / "index.tsv");
  size_t written = 0;
  for (size_t i = 0; i < g_shaderCacheEntryCount; i++) {
    const ShaderCacheEntry& entry = g_shaderCacheEntries[i];
    if (!entry.spirvSize || entry.spirvOffset + entry.spirvSize > spirv.size()) continue;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llX.spv", static_cast<unsigned long long>(entry.hash));
    const uint8_t* smol = spirv.data() + entry.spirvOffset;
    std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
    if (decoded.empty() || !smolv::Decode(smol, entry.spirvSize, decoded.data(), decoded.size())) {
      std::fprintf(stderr, "%s: SMOL-V decode failed\n", entry.filename);
      continue;
    }
    std::ofstream(out / name, std::ios::binary)
        .write(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    index << name << '\t' << entry.filename << '\t' << entry.specConstantsMask << '\t'
          << entry.usedTextureMask << '\n';
    written++;
  }
  std::printf("%zu entries, %zu SPIR-V shaders written (%zu bytes decompressed)\n",
              g_shaderCacheEntryCount, written, spirv.size());
  return 0;
}
