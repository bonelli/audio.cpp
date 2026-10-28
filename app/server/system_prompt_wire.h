#pragma once

#include "engine/framework/io/json.h"
#include "engine/framework/runtime/errors.h"

#include <string>
#include <string_view>
#include <optional>

namespace minitts::server {

// cJSON stores strings without a length, so it truncates embedded NULs. Inspect
// only the actual Drama3 system-prompt field in the already-parsed request.
enum class SystemPromptRequestShape { Root, NestedRequest, Batch };

class SystemPromptWireScan {
public:
    struct EncodingProbe {
        bool valid = true;
        std::optional<engine::io::json::Value> flag;
        std::optional<engine::io::json::Value> model;
        bool request_present = false;
        bool request_is_object = false;
        std::optional<engine::io::json::Value> voice;
        std::optional<engine::io::json::Value> voice_ref;
    };
    SystemPromptWireScan(std::string_view text, SystemPromptRequestShape shape,
                         bool canonical_path_only = false)
        : text_(text), shape_(shape), canonical_path_only_(canonical_path_only) {}

    void validate() {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xef &&
            static_cast<unsigned char>(text_[1]) == 0xbb && static_cast<unsigned char>(text_[2]) == 0xbf) pos_ = 3;
        value(State::Root);
    }
    // Recover only canonical routing fields from a syntactically valid JSON
    // body. NUL-bearing property names are distinct and never select a field.
    EncodingProbe canonical_routing(bool nested_request) {
        probing_ = true;
        routing_phase_ = true;
        shape_ = SystemPromptRequestShape::Root;
        pos_ = 0;
        probe_ = {};
        canonical_models_ = canonical_voices_ = canonical_voice_refs_ = 0;
        canonical_options_ = encoding_fields_ = 0;
        validate();
        routing_phase_ = false;
        pos_ = 0;
        shape_ = nested_request && probe_.request_present ?
            SystemPromptRequestShape::NestedRequest : SystemPromptRequestShape::Root;
        if (nested_request && probe_.request_present && !probe_.request_is_object) {
            probe_.valid = false;
            return probe_;
        }
        validate();
        return probe_;
    }

private:
    enum class State { Root, RootModel, RootRequest, Request, Requests, BatchItem,
                       Options, AmbiguousOptions, AmbiguousOptionsOpaque, Prompt, AmbiguousPromptOpaque,
                       Encoding, Voice, VoiceRef, Ignore };
    struct StringToken { std::string_view raw; bool nul = false; };

    void space() {
        while (pos_ < text_.size() && static_cast<unsigned char>(text_[pos_]) <= 32) ++pos_;
    }

    StringToken string_token() {
        const size_t start = pos_++;
        bool nul = false;
        while (pos_ < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[pos_++]);
            if (ch == '"') return {text_.substr(start, pos_ - start), nul};
            if (ch == 0) nul = true;
            if (ch == '\\' && pos_ < text_.size()) {
                const char escaped = text_[pos_++];
                if (escaped == 'u' && pos_ + 4 <= text_.size()) {
                    if (text_.substr(pos_, 4) == "0000") nul = true;
                    pos_ += 4;
                }
            }
        }
        throw engine::runtime::InvalidRequestError("invalid JSON request");
    }

    State member(State state, const StringToken &key) {
        if (state == State::Ignore || state == State::AmbiguousOptionsOpaque || state == State::AmbiguousPromptOpaque)
            return State::Ignore;
        const auto decoded = engine::io::json::parse(key.raw).as_string();
        if (probing_ && routing_phase_) {
            if (state == State::Root && decoded == "model" && !key.nul) {
                if (++canonical_models_ > 1) probe_.valid = false;
                return State::RootModel;
            }
            if (state == State::Root && decoded == "request" && !key.nul) {
                if (probe_.request_present) probe_.valid = false;
                probe_.request_present = true;
                return State::RootRequest;
            }
            return State::Ignore;
        }
        const bool option_owner = state == State::Request || state == State::BatchItem ||
            (state == State::Root && shape_ == SystemPromptRequestShape::Root);
        if (probing_ && option_owner && decoded == "voice" && !key.nul) {
            if (++canonical_voices_ > 1) probe_.valid = false;
            return State::Voice;
        }
        if (probing_ && option_owner && decoded == "voice_ref" && !key.nul) {
            if (++canonical_voice_refs_ > 1) probe_.valid = false;
            return State::VoiceRef;
        }
        if (option_owner && decoded == "options") {
            if (probing_ && !key.nul && ++canonical_options_ > 1) probe_.valid = false;
            if (!probing_ && canonical_path_only_ && key.nul) return State::AmbiguousOptionsOpaque;
            return key.nul ? State::AmbiguousOptions : State::Options;
        }
        if (probing_ && state == State::Options && decoded == "encode_reference_only" && !key.nul) {
            if (++encoding_fields_ > 1) probe_.valid = false;
            return State::Encoding;
        }
        if ((state == State::Options || state == State::AmbiguousOptions) && decoded == "system_prompt") {
            if (!probing_ && canonical_path_only_ && key.nul) return State::AmbiguousPromptOpaque;
            if (!probing_ && (key.nul || state == State::AmbiguousOptions))
                throw engine::runtime::InvalidRequestError("system_prompt key contains NUL");
            return State::Prompt;
        }
        if (state == State::Root && shape_ == SystemPromptRequestShape::NestedRequest && decoded == "request" &&
            (!(probing_ || canonical_path_only_) || !key.nul))
            return State::Request;
        if (state == State::Root && shape_ == SystemPromptRequestShape::Batch && decoded == "requests")
            return State::Requests;
        return State::Ignore;
    }

    bool value(State state) {
        space();
        if (pos_ >= text_.size()) throw engine::runtime::InvalidRequestError("invalid JSON request");
        const auto start = pos_;
        if (state == State::RootRequest) {
            probe_.request_is_object = text_[pos_] == '{';
            state = State::Ignore;
        }
        if (state == State::VoiceRef) {
            // Presence, including an explicit null, determines preset selection.
            probe_.voice_ref = engine::io::json::Value::make_null();
            state = State::Ignore;
        }
        if (state == State::RootModel && text_[pos_] != '"') {
            probe_.valid = false;
            state = State::Ignore;
        }
        if (state == State::Voice && text_[pos_] != '"') {
            probe_.voice = text_.substr(pos_, 4) == "null" ?
                engine::io::json::Value::make_null() : engine::io::json::Value::make_bool(true);
            state = State::Ignore;
        }
        if (probing_ && state == State::Options && text_[pos_] != '{' && text_.substr(pos_, 4) != "null")
            probe_.valid = false;
        if (text_[pos_] == '"') {
            const auto value = string_token();
            if (state == State::Encoding) probe_.flag = engine::io::json::parse(value.raw);
            if (state == State::RootModel) probe_.model = engine::io::json::parse(value.raw);
            if (state == State::Voice) probe_.voice = engine::io::json::parse(value.raw);
            if (!probing_ && state == State::Prompt && value.nul)
                throw engine::runtime::InvalidRequestError("options.system_prompt cannot contain NUL");
            return state == State::Prompt;
        }
        if (text_[pos_] == '{') {
            if (state == State::Encoding) { probe_.valid = false; state = State::Ignore; }
            ++pos_;
            space();
            if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return state == State::Prompt; }
            bool found_prompt = state == State::Prompt;
            bool ambiguous_options = false;
            bool normal_options_with_prompt = false;
            bool ambiguous_prompt = false;
            bool normal_prompt = false;
            for (;;) {
                space();
                const auto key = string_token();
                space();
                ++pos_; // Colon: the full document has already been validated.
                const auto child_state = member(state, key);
                const bool child_prompt = value(child_state);
                found_prompt = found_prompt || child_prompt;
                ambiguous_options = ambiguous_options || child_state == State::AmbiguousOptions ||
                    child_state == State::AmbiguousOptionsOpaque;
                normal_options_with_prompt = normal_options_with_prompt ||
                    (child_state == State::Options && child_prompt);
                ambiguous_prompt = ambiguous_prompt || child_state == State::AmbiguousPromptOpaque;
                normal_prompt = normal_prompt || child_state == State::Prompt;
                if (!probing_ && ((ambiguous_options && normal_options_with_prompt) ||
                                  (ambiguous_prompt && normal_prompt)))
                    throw engine::runtime::InvalidRequestError("system_prompt key contains NUL");
                space();
                if (text_[pos_++] == '}') return found_prompt;
            }
        }
        if (text_[pos_] == '[') {
            if (state == State::Encoding) { probe_.valid = false; state = State::Ignore; }
            ++pos_;
            space();
            if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return state == State::Prompt; }
            bool found_prompt = state == State::Prompt;
            for (;;) {
                found_prompt = value(state == State::Requests ? State::BatchItem : State::Ignore) || found_prompt;
                space();
                if (text_[pos_++] == ']') return found_prompt;
            }
        }
        while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '}' && text_[pos_] != ']') ++pos_;
        if (state == State::Encoding) probe_.flag = engine::io::json::parse(text_.substr(start, pos_ - start));
        return state == State::Prompt;
    }

    std::string_view text_;
    SystemPromptRequestShape shape_;
    size_t pos_ = 0;
    bool probing_ = false;
    bool routing_phase_ = false;
    bool canonical_path_only_ = false;
    size_t canonical_models_ = 0;
    size_t canonical_voices_ = 0;
    size_t canonical_voice_refs_ = 0;
    size_t canonical_options_ = 0;
    size_t encoding_fields_ = 0;
    EncodingProbe probe_;
};

} // namespace minitts::server
