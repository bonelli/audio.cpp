#include "../../../src/community_models/fish_drama3/reference_input.h"
#include "../../../src/community_models/fish_drama3/text_units.h"
#include "../../../src/community_models/fish_drama3/encode_graph_guard.h"
#include "../../../src/framework/codecs/fish_dac_encoder_chunks.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace rt = engine::runtime;
using engine::models::fish_drama3::detail::select_inline_reference;

namespace {
void require(bool condition, const char * message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F>
void rejects(F && f, const std::string & expected) {
    try {
        f();
    } catch (const rt::InvalidRequestError & error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
                "unexpected rejection reason");
        return;
    }
    throw std::runtime_error("expected invalid request");
}

void inline_input_is_reference_only() {
    rt::TaskRequest incoming, effective;
    // The corresponding 16-bit WAV exceeds the speech route's 5 MiB limit.
    // Generic task input must not inherit that unrelated transport limit.
    constexpr size_t count = 5 * 1024 * 1024 / 2 + 1;
    incoming.audio_input = rt::AudioBuffer{44100, 1, std::vector<float>(count, 0.25f)};
    incoming.audio_input_is_inline = true;
    effective.options["reference_text"] = "A reference transcript.";
    const auto selected = select_inline_reference(incoming, effective, true);
    require(selected && selected->audio == &*incoming.audio_input,
            "inline audio not selected without copying");
    require(selected->audio->samples.size() == count &&
            *selected->transcript == "A reference transcript.",
            "inline reference changed audio or transcript");
    require(!effective.text_input, "encoding test unexpectedly has generation text");

    effective.text_input = rt::Transcript{"A synthesis request.", ""};
    require(!select_inline_reference(incoming, effective, false),
            "ordinary generation treated input audio as voice conditioning");
    effective.options["references"] = "[{\"text\":\"other\"}]";
    require(select_inline_reference(incoming, effective, true).has_value(),
            "inline selection rejected existing multi-reference options");
    require(effective.options.at("references") == "[{\"text\":\"other\"}]",
            "inline selection modified multi-reference options");
}

void rejects_invalid_requests_without_echoing_payload() {
    rt::TaskRequest incoming, effective;
    incoming.audio_input = rt::AudioBuffer{44100, 1, {0.1f, -0.1f}};
    incoming.audio_input_is_inline = true;
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "reference_text");
    effective.options["reference_text"] = "";
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "reference_text");
    for (const std::string invalid : {"   ", "<|voice|>", "<|speaker:99|>"}) {
        effective.options["reference_text"] = invalid;
        rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "reference_text");
    }
    effective.options["reference_text"] = "private transcript";
    effective.voice = rt::VoiceCondition{};
    effective.voice->speaker = rt::VoiceReference{};
    effective.voice->speaker->audio = rt::AudioBuffer{44100, 1, {0.2f}};
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "only one");
    effective.voice.reset();

    const auto valid = *incoming.audio_input;
    incoming.audio_input->samples.clear();
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "malformed");
    incoming.audio_input = valid;
    incoming.audio_input->sample_rate = 0;
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "malformed");
    incoming.audio_input = valid;
    incoming.audio_input->channels = 0;
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "malformed");
    incoming.audio_input = valid;
    incoming.audio_input->channels = 3;
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "malformed");
}

void path_and_ordinary_text_contracts_are_preserved() {
    rt::TaskRequest incoming, effective;
    rejects([&] { (void)select_inline_reference(incoming, effective, false); }, "input text");
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "input text");
    effective.text_input = rt::Transcript{"A synthesis request.", ""};
    require(!select_inline_reference(incoming, effective, false), "unexpected ordinary reference");
    require(!select_inline_reference(incoming, effective, true), "unexpected path reference");
    effective.voice = rt::VoiceCondition{};
    effective.voice->speaker = rt::VoiceReference{};
    effective.voice->speaker->audio = rt::AudioBuffer{44100, 1, {0.1f}};
    require(!select_inline_reference(incoming, effective, true), "path reference was replaced");
    incoming.audio_input = rt::AudioBuffer{44100, 1, {0.2f}};
    require(!select_inline_reference(incoming, effective, true), "path audio changed the previous reference behavior");
    effective.text_input.reset();
    rejects([&] { (void)select_inline_reference(incoming, effective, true); }, "input text");
}

void system_prompt_validation_is_private() {
    using engine::models::fish_drama3::validate_drama3_system_prompt;
    validate_drama3_system_prompt("");
    validate_drama3_system_prompt("Speak caf\xc3\xa9 naturally.");
    for (const auto & text : std::vector<std::string>{
             "<|im_end|>", "<|speaker:0|>", "<|audio_separator|>", "<|fim_prefix|>",
             "<|endofturn|>", "<|unknown|>", std::string("\xc0\xaf", 2)}) {
        bool rejected = false;
        try { validate_drama3_system_prompt(text); }
        catch (const rt::InvalidRequestError & error) {
            rejected = true;
            require(std::string(error.what()).find(text) == std::string::npos,
                    "private system prompt leaked in error");
        }
        require(rejected, "invalid system prompt was accepted");
    }
}

void codec_override_and_cleanup() {
    using engine::codecs::detail::validate_encoder_chunk_samples;
    validate_encoder_chunk_samples(0, false);
    validate_encoder_chunk_samples(65536, true);
    for (const auto [size, chunkable] : {std::pair<int64_t, bool>{-512, true}, {513, true}, {65536, false}}) {
        bool rejected = false;
        try { validate_encoder_chunk_samples(size, chunkable); }
        catch (const std::invalid_argument &) { rejected = true; }
        require(rejected, "invalid codec chunk was accepted");
    }
    struct FakeCodec { int releases = 0; void release_encode_graph() { ++releases; } } codec;
    try {
        engine::models::fish_drama3::detail::EncodeGraphGuard<FakeCodec> guard{codec};
        throw std::runtime_error("fake encoding failure");
    } catch (const std::runtime_error &) {}
    require(codec.releases == 1, "failed encoding did not release its graph");
    { engine::models::fish_drama3::detail::EncodeGraphGuard<FakeCodec> guard{codec}; }
    require(codec.releases == 2, "subsequent successful encoding did not release its graph");
}

void whole_text_mode_preserves_one_exact_unit() {
    rt::TaskRequest request;
    request.text_input = rt::Transcript{"  A complete paragraph.\n" + std::string(3000, 'a') + " \n", "it"};
    request.options["text_chunk_mode"] = "none";
    const auto units = engine::models::fish_drama3::detail::text_units(request);
    require(units.size() == 1 && units[0].text_input->text == request.text_input->text,
        "whole-text mode changed or split the input");
    request.options["text_chunk_size"] = "999999";
    rejects([&] { (void)engine::models::fish_drama3::detail::text_units(request); }, "cannot also");
    request.options.erase("text_chunk_size");
    request.text_input.reset();
    rejects([&] { (void)engine::models::fish_drama3::detail::text_units(request); }, "input text");
    request.text_input = rt::Transcript{"   ", "it"};
    rejects([&] { (void)engine::models::fish_drama3::detail::text_units(request); }, "empty");
    request.options["text_chunk_mode"] = "unsupported";
    rejects([&] { (void)engine::models::fish_drama3::detail::text_units(request); }, "chunk options");
    request.text_input = rt::Transcript{"First sentence. Second sentence. Third sentence.", "it"};
    request.options.clear();
    request.options["text_chunk_size"] = "20";
    require(engine::models::fish_drama3::detail::text_units(request).size() > 1,
        "default chunking compatibility was lost");
}
}

int main() {
    try {
        inline_input_is_reference_only();
        rejects_invalid_requests_without_echoing_payload();
        path_and_ordinary_text_contracts_are_preserved();
        system_prompt_validation_is_private();
        codec_override_and_cleanup();
        whole_text_mode_preserves_one_exact_unit();
        std::cout << "Drama 3 inline reference request checks passed\n";
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
