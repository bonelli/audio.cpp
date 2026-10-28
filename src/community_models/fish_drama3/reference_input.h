#pragma once

#include "engine/framework/runtime/errors.h"
#include "engine/framework/runtime/session.h"
#include "engine/community_models/fish_drama3/prompt.h"

#include <optional>
#include <string>

namespace engine::models::fish_drama3::detail {

// The generic task endpoint carries inline WAV in audio_input. Only the
// reference-only operation interprets it as voice conditioning.
struct InlineReferenceInput {
    const runtime::AudioBuffer * audio;
    const std::string * transcript;
};

inline std::optional<InlineReferenceInput> select_inline_reference(
    const runtime::TaskRequest & incoming,
    const runtime::TaskRequest & effective,
    bool encode_reference_only) {
    if (!encode_reference_only || !incoming.audio_input || !incoming.audio_input_is_inline) {
        if (!effective.text_input) {
            throw runtime::InvalidRequestError("Drama 3 requires input text");
        }
        return std::nullopt;
    }
    if (effective.voice && effective.voice->speaker && effective.voice->speaker->audio) {
        throw runtime::InvalidRequestError("provide only one of audio input or voice_ref for reference encoding");
    }
    const auto transcript = effective.options.find("reference_text");
    if (transcript == effective.options.end() || transcript->second.empty()) {
        throw runtime::InvalidRequestError("Drama 3 reference audio requires reference_text");
    }
    validate_drama3_text(transcript->second, "reference_text");
    const auto & audio = *incoming.audio_input;
    if (audio.sample_rate <= 0 || audio.channels <= 0 || audio.samples.empty() ||
        audio.samples.size() % static_cast<size_t>(audio.channels) != 0) {
        throw runtime::InvalidRequestError("Drama 3 reference audio is empty or malformed");
    }
    return InlineReferenceInput{&audio, &transcript->second};
}

}  // namespace engine::models::fish_drama3::detail
