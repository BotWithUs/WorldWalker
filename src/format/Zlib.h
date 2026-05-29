#ifndef WORLDWALKER_FORMAT_ZLIB_H
#define WORLDWALKER_FORMAT_ZLIB_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Thin zlib wrappers shared by the artifact writer (wwbuild) and the runtime
// reader (worldwalker). Exported from worldwalker.dll; both binaries are built
// from the same toolchain/config and share the CRT DLL heap, so returning a
// std::vector across the DLL boundary is safe here.
namespace ww::format
{
    // Compress `size` bytes at `input` (level 0..9; 6 is a sensible default).
    // Throws std::runtime_error on a zlib error or if size overflows zlib's
    // 32-bit length.
    std::vector<uint8_t> zlibCompress(const uint8_t *input, std::size_t size, int level = 6);

    // Decompress a zlib stream into exactly `rawSize` bytes. Throws
    // std::runtime_error on a zlib error or a size mismatch.
    std::vector<uint8_t> zlibDecompress(const uint8_t *input, std::size_t size, std::size_t rawSize);
}

#endif  // WORLDWALKER_FORMAT_ZLIB_H
