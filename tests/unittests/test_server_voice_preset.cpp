// A voice preset's options reach the model as request options: they fill what the
// request leaves unset, for named and default presets alike, and the server never
// interprets them.
#include "server_protocol.h"
#include "../../app/cli/request.h"

#include "engine/framework/io/json.h"
#include "engine/framework/runtime/errors.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using minitts::server::RuntimeVoicePreset;
using minitts::server::SpeechRequestContext;

void expect(bool condition, const std::string & message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

engine::runtime::TaskRequest speech(
    const std::unordered_map<std::string, RuntimeVoicePreset> & presets,
    const std::optional<RuntimeVoicePreset> & default_preset,
    const std::unordered_map<std::string, std::string> & default_options,
    const std::string & body_text) {
    const fs::path base = fs::current_path();
    const std::optional<fs::path> voice_dir;
    return minitts::server::build_speech_request(
        engine::io::json::parse(body_text),
        SpeechRequestContext{base, voice_dir, presets, default_preset, true, true, true, default_options});
}

}  // namespace

int main() {
    try {
        minitts::server::ServerModelConfig model;
        model.family = "fish_drama3";
        const std::unordered_map<std::string, std::string> inherited{{"system_prompt", "old preset"}};
        for (const auto & value : {std::optional<std::string>{}, std::optional<std::string>{""},
                                   std::optional<std::string>{"caller"}}) {
            const auto body = engine::io::json::parse(value ?
                "{\"text\":\"hello\",\"options\":{\"system_prompt\":\"" + *value + "\"}}" :
                "{\"text\":\"hello\"}");
            auto request = minitts::cli::build_request_from_json(body, fs::current_path());
            request = minitts::server::apply_default_request_options(inherited, std::move(request));
            request = minitts::server::preserve_request_scoped_drama3_system_prompt(std::move(request), body, model);
            expect(request.options.count("system_prompt") == (value ? 1u : 0u), "task inherited instruction");
            if (value) expect(request.options.at("system_prompt") == *value, "task explicit instruction changed");
        }
        RuntimeVoicePreset legacy_preset;
        legacy_preset.options = inherited;
        const std::unordered_map<std::string, RuntimeVoicePreset> legacy_presets{{"legacy", legacy_preset}};
        for (const auto & value : {std::optional<std::string>{}, std::optional<std::string>{""},
                                   std::optional<std::string>{"caller"}}) {
            const std::string raw = value ?
                "{\"input\":\"hello\",\"voice\":\"legacy\",\"options\":{\"system_prompt\":\"" + *value + "\"}}" :
                "{\"input\":\"hello\",\"voice\":\"legacy\"}";
            const auto body = engine::io::json::parse(raw);
            auto request = speech(legacy_presets, {}, inherited, raw);
            request = minitts::server::preserve_request_scoped_drama3_system_prompt(std::move(request), body, model);
            expect(request.options.count("system_prompt") == (value ? 1u : 0u), "speech inherited instruction");
            if (value) expect(request.options.at("system_prompt") == *value, "speech explicit instruction changed");
        }
        {
            const auto body = engine::io::json::parse("{\"text\":\"hello\"}");
            auto request = minitts::cli::build_request_from_json(body, fs::current_path());
            request = minitts::server::apply_default_request_options(inherited, std::move(request));
            model.family = "another_family";
            request = minitts::server::preserve_request_scoped_drama3_system_prompt(std::move(request), body, model);
            expect(request.options.at("system_prompt") == "old preset", "other model default changed");
            model.family = "fish_drama3";
        }
        {
            const auto reference_request = engine::io::json::parse(
                R"({"options":{"encode_reference_only":true,"system_prompt":123}})");
            expect(minitts::server::is_drama3_reference_only(reference_request, model), "encode-only flag lost");
            minitts::server::validate_drama3_system_prompt(reference_request, model);
            const std::string raw = R"({"options":{"encode_reference_only":true,"system_prompt":"a\u0000b"}})";
            minitts::server::validate_drama3_system_prompt_wire(raw, engine::io::json::parse(raw), model, false, false);
            model.default_request_options["encode_reference_only"] = "true";
            minitts::server::validate_drama3_system_prompt(
                engine::io::json::parse(R"({"options":{"system_prompt":123}})"), model);
            expect(!minitts::server::is_drama3_reference_only(
                engine::io::json::parse(R"({"options":{"encode_reference_only":""}})"), model),
                "empty encode override must mask the default");
            expect(!minitts::server::is_drama3_reference_only(
                engine::io::json::parse(R"({"options":{"encode_reference_only":0}})"), model),
                "numeric encode override must preserve conversion");
            expect(minitts::server::is_drama3_reference_only(
                engine::io::json::parse(R"({"options":{"encode_reference_only":1,"references":{"nested":[1,2,3]}}})"), model),
                "numeric encode flag or unrelated options changed");
            bool invalid_flag = false;
            try {
                (void) minitts::server::is_drama3_reference_only(
                    engine::io::json::parse(R"({"options":{"encode_reference_only":null}})"), model);
            } catch (const engine::runtime::InvalidRequestError &) { invalid_flag = true; }
            expect(invalid_flag, "null encode flag should retain prior conversion error");
            model.default_request_options.clear();
            model.voice_presets["encoder"].options["encode_reference_only"] = "true";
            const auto speech_reference = engine::io::json::parse(R"({"voice":"encoder","options":{"system_prompt":123}})");
            expect(minitts::server::is_drama3_reference_only(speech_reference, model, true), "speech encode preset lost");
            minitts::server::validate_drama3_system_prompt(speech_reference, model, true);
            expect(!minitts::server::is_drama3_reference_only(speech_reference, model), "task applied speech preset");
            const auto override_request = engine::io::json::parse(
                R"({"voice":"encoder","options":{"encode_reference_only":false,"system_prompt":123}})");
            bool rejected = false;
            try { minitts::server::validate_drama3_system_prompt(override_request, model, true); }
            catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
            expect(rejected, "explicit synthesis override ignored");
            model.voice_presets.clear();
        }
        {
            const auto lookup = [&](const engine::io::json::Value &) { return std::optional{model}; };
            for (const auto & raw : {
                R"({"model":"test","request":{"options":{"system_prompt":"one","system_prompt\u0000extra":"two"}}})",
                R"({"model":"test","request":{"options":{"system_prompt":"one"},"options\u0000extra":{}}})",
                R"({"model":"test","request":{"options\u0000extra":{},"options":{"system_prompt":"one"}}})"
            }) {
                bool rejected = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &error) {
                    rejected = true;
                    expect(std::string(error.what()).find("one") == std::string::npos, "collision error echoed input");
                }
                expect(rejected, "ambiguous Drama3 prompt key not rejected");
            }
            const auto untouched = R"({"model":"test","request":{"options":{"system_prompt":"one","system_prompt\u0000extra":"two"}}})";
            model.family = "another_family";
            bool original_error = false;
            try { (void) minitts::server::parse_task_body_guarded(untouched, lookup, false, true); }
            catch (const engine::runtime::InvalidRequestError &) {}
            catch (const std::runtime_error &) { original_error = true; }
            expect(original_error, "other model parse classification changed");
            model.family = "fish_drama3";
            for (const auto & raw : {
                R"({"model":"test","request":{"options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{"encode_reference_only":false}}})",
                R"({"model":"test","request":{"options\u0000suffix":{"encode_reference_only":false},"options":{"system_prompt":"x\u0000y"}}})"
            }) {
                bool rejected = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &error) {
                    rejected = true;
                    expect(std::string(error.what()).find("x") == std::string::npos,
                           "NUL collision error echoed input");
                }
                expect(rejected, "false encoder flag in ambiguous options masked system_prompt NUL");
                model.default_request_options["encode_reference_only"] = "true";
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "uncertain encoder default changed parse classification");
                model.default_request_options.clear();
            }
            for (const auto & raw : {
                R"({"model":"test","request":{"options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{"encode_reference_only":true}}})",
                R"({"model":"test","request":{"options\u0000suffix":{"encode_reference_only":true},"options":{"system_prompt":"x\u0000y"}}})"
            }) {
                bool rejected = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
                expect(rejected, "noncanonical true encoder flag masked system prompt NUL");
                model.default_request_options["encode_reference_only"] = "true";
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "canonical encoder default changed parse classification");
                model.default_request_options.clear();
            }
            for (const auto & raw : {
                R"({"model":"test","request":{"options":{"encode_reference_only":true,"system_prompt":"x\u0000y"},"options\u0000suffix":{"encode_reference_only":false}}})",
                R"({"model":"test","request":{"options\u0000suffix":{"encode_reference_only":false},"options":{"encode_reference_only":true,"system_prompt":"x\u0000y"}}})"
            }) {
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "explicit canonical encoder mode changed parse classification");
            }
            model.default_request_options["encode_reference_only"] = "true";
            for (const auto & raw : {
                R"({"model":"test","request":{"options":{"encode_reference_only":false,"system_prompt":"x\u0000y"},"options\u0000suffix":{"encode_reference_only":true}}})",
                R"({"model":"test","request":{"options\u0000suffix":{"encode_reference_only":true},"options":{"encode_reference_only":false,"system_prompt":"x\u0000y"}}})",
                R"({"model":"test","request":{"opt\u0069ons":{"encode_reference_only":false,"system_prompt":"x\u0000y"},"options\u0000suffix":{"encode_reference_only":true}}})"
            }) {
                bool rejected = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
                expect(rejected, "explicit canonical synthesis mode was masked");
            }
            model.default_request_options.clear();
            for (const auto & raw : {
                R"({"model":"test","request":{"options":{"system_prompt":"x\u0000y"},"options":{"encode_reference_only":false}}})",
                R"({"model":"test","request":{"options":{"encode_reference_only":false,"encode_reference_only":true,"system_prompt":"x\u0000y"}}})"
            }) {
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "duplicate canonical key changed parse classification");
            }
            for (const auto & raw : {
                R"({"model":"test","model\u0000suffix":"other","request":{"options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}}})",
                R"({"model\u0000suffix":"other","model":"test","request":{"options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}}})",
                R"({"model":"test","request\u0000suffix":{},"request":{"options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}}})",
                R"({"model":"test","request":{"options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}},"request\u0000suffix":{}})"
            }) {
                bool rejected = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
                expect(rejected, "noncanonical routing key masked synthesis instruction NUL");
            }
            {
                const auto missing_model = R"({"model\u0000suffix":"test","request":{"options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}}})";
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(missing_model, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "noncanonical model key was treated as canonical");
            }
            for (const auto & raw : {
                R"({"model":"test","request":{"options":{"system_prompt":"safe"}},"request\u0000suffix":{"options":{"system_prompt":"x\u0000y"}}})",
                R"({"model":"test","request\u0000suffix":{"options":{"system_prompt":"x\u0000y"}},"request":{"options":{"system_prompt":"safe"}}})",
                R"({"model":"test","request":{"options":{"system_prompt":"safe","encode_reference_only":false}},"request\u0000suffix":{"options":{"system_prompt":"x\u0000y","encode_reference_only":true}}})",
                R"({"model":"test","request":{"options\u0000suffix":{"system_prompt":"x\u0000y"},"other":1,"other\u0000suffix":2}})",
                R"({"model":"test","request":{"options":{"system_prompt\u0000suffix":"x\u0000y","other":1,"other\u0000suffix":2}}})"
            }) {
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "noncanonical request changed canonical instruction validation");
            }
            {
                const auto raw = R"({"model":"test","input":"hi","request":{"options":{"system_prompt":"x\u0000y"}},"options":{"seed":5},"options\u0000suffix":{}})";
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "speech incorrectly treated nested request as its instruction");
            }
            {
                const auto raw = R"({"model":"test","request\u0000suffix":{"options":{"system_prompt":"x\u0000y"}}})";
                const auto parsed = engine::io::json::parse(raw);
                bool rejected = false;
                try { minitts::server::validate_drama3_system_prompt_wire(raw, parsed, model, false, true); }
                catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
                expect(rejected, "ordinary cJSON request alias allowed NUL instruction truncation");
            }
            {
                const auto speech = R"({"model":"test","input":"hi","voice":"sample","options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{"encode_reference_only":true}})";
                minitts::server::ServerModelConfig::VoicePreset preset;
                preset.options["encode_reference_only"] = "false";
                model.default_request_options["encode_reference_only"] = "true";
                model.voice_presets["sample"] = preset;
                bool rejected = false;
                try { (void) minitts::server::parse_task_body_guarded(speech, lookup, false, false, true); }
                catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
                expect(rejected, "canonical voice preset did not override encoder default");
                model.voice_presets["sample"].options["encode_reference_only"] = "true";
                model.default_request_options["encode_reference_only"] = "false";
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(speech, lookup, false, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "encoder voice preset changed parse classification");
                model.default_request_options.clear();
                model.voice_presets.clear();
            }
            {
                minitts::server::ServerModelConfig::VoicePreset synth;
                minitts::server::ServerModelConfig::VoicePreset encoder;
                synth.options["encode_reference_only"] = "false";
                encoder.options["encode_reference_only"] = "true";
                model.voice_presets["synth"] = synth;
                model.voice_presets["encoder"] = encoder;
                model.default_request_options["encode_reference_only"] = "false";
                for (const auto & raw : {
                    R"({"model":"test","input":"hi","voice\u0000suffix":"encoder","options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}})",
                    R"({"model":"test","input":"hi","voice":"synth","voice\u0000suffix":"encoder","options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}})",
                    R"({"model":"test","input":"hi","voice\u0000suffix":"encoder","voice":"synth","options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}})"
                }) {
                    bool rejected = false;
                    try { (void) minitts::server::parse_task_body_guarded(raw, lookup, false, false, true); }
                    catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
                    expect(rejected, "noncanonical voice key masked synthesis instruction NUL");
                }
                model.default_voice_preset_id = "encoder";
                const auto noncanonical_ref = R"({"model":"test","input":"hi","voice_ref\u0000suffix":"ignored","options":{"system_prompt":"x\u0000y"},"options\u0000suffix":{}})";
                bool original_error = false;
                try { (void) minitts::server::parse_task_body_guarded(noncanonical_ref, lookup, false, false, true); }
                catch (const engine::runtime::InvalidRequestError &) {}
                catch (const std::runtime_error &) { original_error = true; }
                expect(original_error, "noncanonical voice_ref changed default encoder selection");
                model.default_voice_preset_id.reset();
                model.default_request_options.clear();
                model.voice_presets.clear();
            }
            const auto unrelated = R"({"model":"test","request":{"options":{"different":"one","different\u0000extra":"two"}}})";
            original_error = false;
            try { (void) minitts::server::parse_task_body_guarded(unrelated, lookup, false, true); }
            catch (const engine::runtime::InvalidRequestError &) {}
            catch (const std::runtime_error &) { original_error = true; }
            expect(original_error, "unrelated collision classification changed");
            const auto encode = R"({"model":"test","request":{"options":{"encode_reference_only":true,"system_prompt":"one","system_prompt\u0000extra":"two"}}})";
            original_error = false;
            try { (void) minitts::server::parse_task_body_guarded(encode, lookup, false, true); }
            catch (const engine::runtime::InvalidRequestError &) {}
            catch (const std::runtime_error &) { original_error = true; }
            expect(original_error, "encoder-only parse classification changed");
            const auto valid = R"({"model":"test","request":{"options":{"system_prompt":"valid"}}})";
            expect(minitts::server::parse_task_body_guarded(valid, lookup, false, true).is_object(), "valid request changed");
        }
        for (const auto & raw : {"{}", "{\"options\":{\"system_prompt\":\"\"}}", "{\"options\":{\"system_prompt\":\"ordinary\"}}"}) {
            minitts::server::validate_drama3_system_prompt(engine::io::json::parse(raw), model);
        }
        model.family = "another_family";
        minitts::server::validate_drama3_system_prompt(
            engine::io::json::parse("{\"options\":{\"system_prompt\":123}}"), model);
        for (const auto & value : {"", "Speak clearly"}) {
            const std::string raw = "{\"input\":\"hello\",\"options\":{\"system_prompt\":\"" + std::string(value) + "\"}}";
            const auto speech_request = speech({}, {}, {}, raw);
            expect(speech_request.options.at("system_prompt") == value, "speech system_prompt changed");
            const auto task_request = minitts::cli::build_request_from_json(
                engine::io::json::parse("{\"text\":\"hello\",\"options\":{\"system_prompt\":\"" + std::string(value) + "\"}}"),
                fs::current_path());
            expect(task_request.options.at("system_prompt") == value, "task system_prompt changed");
        }
        model.family = "fish_drama3";
        for (const auto & [raw, batch, nested, rejected] :
             std::vector<std::tuple<std::string, bool, bool, bool>>{
                {R"({"options":{"system_prompt":"a\u0000b"}})", false, false, true},
                {std::string("{\"options\":{\"system_prompt\":\"a") + '\0' + "b\"}}", false, false, true},
                {std::string("\xef\xbb\xbf") + R"({"options":{"system_prompt":"a\u0000b"}})", false, false, true},
                {R"({"request":{"options":{"system_\u0070rompt":"a\u0000b"}}})", false, true, true},
                {R"({"requests":[{"options":{"system_prompt":"a\u0000b"}}]})", true, false, false},
                {R"({"options":{"system_prompt\u0000extra":"a"}})", false, false, true},
                {R"({"options\u0000extra":{"system_prompt":"a"}})", false, false, true},
                {R"({"options\u0000extra":{"different":"a"}})", false, false, false},
                {R"({"options":{"system_prompt":"literal \\u0000"}})", false, false, false},
                {R"({"options":{"system_prompt":"quote \" and backslash \\ then normal"}})", false, false, false},
                {R"({"text":"a\u0000b","options":{"system_prompt":"normal"}})", false, false, false},
                {R"({"meta":{"options":{"system_prompt":"a\u0000b"}},"options":{"system_prompt":"normal"}})", false, false, false},
                {R"({"request":{"options":{"system_prompt":"normal"}},"options":{"system_prompt":"a\u0000b"}})", false, true, false},
                {R"({"options":{"different\u0000key":"a"}})", false, false, false},
             }) {
            bool thrown = false;
            try { minitts::server::validate_drama3_system_prompt_wire(raw, engine::io::json::parse(raw), model, batch, nested); }
            catch (const std::exception & error) {
                thrown = true;
                expect(std::string(error.what()).find("a\\u0000b") == std::string::npos, "NUL error leaked value");
            }
            expect(thrown == rejected, "wrong field was affected by system_prompt NUL guard");
        }
        model.family = "another_family";
        const std::string other = R"({"options":{"system_prompt":"a\u0000b"}})";
        minitts::server::validate_drama3_system_prompt_wire(other, engine::io::json::parse(other), model, false, false);
        RuntimeVoicePreset narrator;
        narrator.options = {
            {"speaker_embedding_file", "/voices/narrator.speaker.safetensors"},
            {"num_steps", "8"},
        };
        RuntimeVoicePreset with_text;
        with_text.reference_text = "preset transcript";
        with_text.options = {{"reference_text", "option transcript"}};
        const std::unordered_map<std::string, RuntimeVoicePreset> presets{
            {"narrator", narrator},
            {"with_text", with_text},
        };
        const std::optional<RuntimeVoicePreset> no_default;
        const std::unordered_map<std::string, std::string> no_defaults;

        auto request = speech(presets, no_default, no_defaults, R"({"input":"x","voice":"narrator"})");
        expect(request.options.at("speaker_embedding_file") == "/voices/narrator.speaker.safetensors",
               "preset option did not reach the request");
        expect(request.options.at("num_steps") == "8", "second preset option did not reach the request");
        expect(!request.voice.has_value(), "an options-only preset should not add a voice");

        // The request's own options win over the preset's.
        request = speech(presets, no_default, no_defaults,
            R"({"input":"x","voice":"narrator","options":{"num_steps":4}})");
        expect(request.options.at("num_steps") == "4", "request option did not override the preset");
        expect(request.options.at("speaker_embedding_file") == "/voices/narrator.speaker.safetensors",
               "unrelated preset option was dropped");

        // The preset wins over the model's default_request_options.
        const std::unordered_map<std::string, std::string> defaults{{"num_steps", "32"}, {"cfg_scale", "2"}};
        request = speech(presets, no_default, defaults, R"({"input":"x","voice":"narrator"})");
        expect(request.options.at("num_steps") == "8", "default_request_options overrode the preset");
        expect(request.options.at("cfg_scale") == "2", "default_request_options were dropped");

        // The dedicated reference_text field keeps its meaning.
        request = speech(presets, no_default, no_defaults, R"({"input":"x","voice":"with_text"})");
        expect(request.options.at("reference_text") == "preset transcript",
               "preset reference_text should win over the same key in its options");

        // A default preset carries its options too, until the request names a voice.
        const std::optional<RuntimeVoicePreset> default_preset = narrator;
        request = speech(presets, default_preset, no_defaults, R"({"input":"x"})");
        expect(request.options.at("speaker_embedding_file") == "/voices/narrator.speaker.safetensors",
               "default preset options did not reach the request");
        request = speech(presets, default_preset, no_defaults, R"({"input":"x","voice":"alba"})");
        expect(request.options.count("speaker_embedding_file") == 0,
               "default preset options leaked into a request that named another voice");
        expect(request.voice.has_value() && request.voice->speaker.has_value() &&
                   request.voice->speaker->cached_voice_id == "alba",
               "a voice that is not a preset should stay a model-native voice id");

        RuntimeVoicePreset clone;
        clone.audio = engine::runtime::AudioBuffer{24000, 1, {0.25F, -0.25F}};
        clone.reference_text = "reference transcript";
        for (const std::string body : {R"({"input":"x"})",
                                       R"({"input":"x","voice":""})",
                                       R"({"input":"x","voice":null})"}) {
            request = speech(presets, clone, no_defaults, body);
            expect(request.voice.has_value() && request.voice->speaker.has_value() &&
                       request.voice->speaker->audio.has_value(),
                   "omitted, empty, or null voice should retain the default reference");
            expect(request.voice->speaker->audio->samples == clone.audio->samples &&
                       request.options.at("reference_text") == *clone.reference_text,
                   "default reference audio or transcript changed");
            request = speech(presets, no_default, no_defaults, body);
            expect(!request.voice.has_value(), "empty or null voice should not create a voice");
        }
        request = speech(presets, clone, no_defaults, R"({"input":"x","voice":"alba"})");
        expect(request.voice->speaker->cached_voice_id == "alba" &&
                   !request.voice->speaker->audio.has_value(),
               "nonempty voice must still override the default reference");
        bool rejected = false;
        try { (void)speech(presets, clone, no_defaults, R"({"input":"x","voice":123})"); }
        catch (const std::runtime_error &) { rejected = true; }
        expect(rejected, "non-string, non-null voice must still be rejected");

        // Instruction aliases follow request > preset > model precedence.
        for (const std::string key : {"instruct", "instruction"}) {
            const std::string alias = key == "instruct" ? "instruction" : "instruct";
            RuntimeVoicePreset styled = narrator;
            styled.options[key] = "calm";
            const std::unordered_map<std::string, RuntimeVoicePreset> styled_presets{{"styled", styled}};
            const std::unordered_map<std::string, std::string> styled_defaults{{alias, "serious"}};
            auto check = [&](const std::string & body, const std::optional<RuntimeVoicePreset> & selected,
                             const std::string & expected_key, const std::string & expected_value) {
                const auto result = speech(styled_presets, selected, styled_defaults, body);
                expect(result.options.at(expected_key) == expected_value, "instruction precedence changed");
                expect(result.options.count(expected_key == "instruct" ? "instruction" : "instruct") == 0,
                       "lower-priority instruction alias survived");
                expect(result.options.at("num_steps") == "8", "unrelated preset option changed");
                expect(result.options.at("speaker_embedding_file") == "/voices/narrator.speaker.safetensors",
                       "unrelated voice option changed");
            };
            check(R"({"input":"x","voice":"styled"})", no_default, key, "calm");
            check(R"({"input":"x"})", styled, key, "calm");
            for (const std::string request_key : {key, alias}) {
                check("{\"input\":\"x\",\"voice\":\"styled\",\"options\":{\"" + request_key + "\":\"cheerful\"}}",
                      no_default, request_key, "cheerful");
                check("{\"input\":\"x\",\"options\":{\"" + request_key + "\":\"cheerful\"}}",
                      styled, request_key, "cheerful");
                const std::string field = request_key == "instruct" ? "instruct" : "instructions";
                check("{\"input\":\"x\",\"voice\":\"styled\",\"" + field + "\":\"cheerful\"}",
                      no_default, request_key, "cheerful");
            }
        }
    } catch (const std::exception & error) {
        std::cerr << "voice preset test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "server voice preset passed\n";
    return 0;
}
