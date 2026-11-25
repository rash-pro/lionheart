#ifndef FF8_HOST_DDS_DECODE_HPP
#define FF8_HOST_DDS_DECODE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HostHD {

struct DdsImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

/* Clean-room MIT CPU decoder for DDS images.
 * Supports BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC7 (DX10 DXGI_FORMAT_BC7_UNORM / SRGB,
 * all 8 BPTC modes), and uncompressed 24/32-bit RGB/BGR/RGBA/BGRA. */
bool decode_dds(const uint8_t *data, size_t size, DdsImage &out, std::string *error = nullptr);

} // namespace HostHD

#endif
