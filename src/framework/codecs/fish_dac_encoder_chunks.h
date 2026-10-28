#pragma once
#include <cstdint>
#include <stdexcept>

namespace engine::codecs::detail {
inline void validate_encoder_chunk_samples(int64_t chunk, bool chunkable) {
    if (chunk < 0 || chunk % 512 != 0)
        throw std::invalid_argument("Fish DAC codec encoder_chunk_samples must be 0 or a positive multiple of 512 samples");
    if (chunk > 0 && !chunkable)
        throw std::invalid_argument("Fish DAC codec chunked encoding requires hop-aligned frames and a convolution stack with no intermediate transformers");
}
}
