#include "format/Zlib.h"

#include <zlib.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace ww::format
{
    namespace
    {
        [[noreturn]] void fail(const char *what, int code)
        {
            throw std::runtime_error(std::string(what) + ": zlib code " + std::to_string(code));
        }
    }

    std::vector<uint8_t> zlibCompress(const uint8_t *input, std::size_t size, int level)
    {
        if (size > std::numeric_limits<uLong>::max())
        {
            throw std::runtime_error("zlibCompress: input too large for zlib");
        }
        uLongf produced = ::compressBound(static_cast<uLong>(size));
        std::vector<uint8_t> out(produced);
        int rc = ::compress2(out.data(), &produced, input, static_cast<uLong>(size), level);
        if (rc != Z_OK)
        {
            fail("zlibCompress", rc);
        }
        out.resize(produced);
        return out;
    }

    std::vector<uint8_t> zlibDecompress(const uint8_t *input, std::size_t size, std::size_t rawSize)
    {
        std::vector<uint8_t> out(rawSize);
        zlibDecompressInto(input, size, out.data(), rawSize);
        return out;
    }

    void zlibDecompressInto(const uint8_t *input, std::size_t size,
                            uint8_t *output, std::size_t rawSize)
    {
        if (size > std::numeric_limits<uLong>::max() || rawSize > std::numeric_limits<uLong>::max())
        {
            throw std::runtime_error("zlibDecompressInto: size too large for zlib");
        }
        uLongf produced = static_cast<uLongf>(rawSize);
        int rc = ::uncompress(output, &produced, input, static_cast<uLong>(size));
        if (rc != Z_OK)
        {
            fail("zlibDecompressInto", rc);
        }
        if (produced != rawSize)
        {
            throw std::runtime_error("zlibDecompressInto: produced " + std::to_string(produced)
                                     + " bytes, expected " + std::to_string(rawSize));
        }
    }
}
