#pragma once

#include "engine/community_models/fish_drama3/prompt.h"
#include "engine/framework/runtime/options.h"
#include "engine/framework/runtime/errors.h"
#include "engine/framework/text/chunking.h"

namespace engine::models::fish_drama3::detail {

inline bool single_text_unit(const runtime::TaskRequest & request) {
    const auto mode = request.options.find("text_chunk_mode");
    return mode != request.options.end() && mode->second == "none";
}

inline std::vector<runtime::TaskRequest> text_units(const runtime::TaskRequest & request) {
    if (single_text_unit(request)) {
        if (request.options.count("text_chunk_size")) {
            throw runtime::InvalidRequestError("text_chunk_mode=none cannot also specify text_chunk_size");
        }
        if (!request.text_input) throw runtime::InvalidRequestError("Drama 3 requires input text");
        validate_drama3_text(request.text_input->text, "input");
        return {request};
    }
    try {
        const auto size = runtime::parse_positive_i64_option(request.options, {"text_chunk_size"}, 128);
        const auto mode = engine::text::parse_text_chunk_mode_override(request.options)
            .value_or(engine::text::TextChunkMode::TagAware);
        return runtime::chunk_text_request(request, size, mode);
    } catch (const std::runtime_error &) {
        throw runtime::InvalidRequestError("Invalid Drama 3 text chunk options");
    }
}

}  // namespace engine::models::fish_drama3::detail
