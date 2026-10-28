#pragma once

#include "engine/framework/io/json.h"
#include "engine/framework/runtime/errors.h"

#include <string>
#include <string_view>

namespace minitts::server {

// cJSON stores strings without a length, so it truncates embedded NULs. Inspect
// only the actual Drama3 system-prompt field in the already-parsed request.
enum class SystemPromptRequestShape { Root, NestedRequest, Batch };

class SystemPromptWireScan {
public:
    SystemPromptWireScan(std::string_view text, SystemPromptRequestShape shape)
        : text_(text), shape_(shape) {}

    void validate() {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xef &&
            static_cast<unsigned char>(text_[1]) == 0xbb && static_cast<unsigned char>(text_[2]) == 0xbf) pos_ = 3;
        value(State::Root);
    }

private:
    enum class State { Root, Request, Requests, BatchItem, Options, AmbiguousOptions, Prompt, Ignore };
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

    State member(State state, const StringToken &key) const {
        if (state == State::Ignore) return state;
        const auto decoded = engine::io::json::parse(key.raw).as_string();
        const bool option_owner = state == State::Request || state == State::BatchItem ||
            (state == State::Root && shape_ == SystemPromptRequestShape::Root);
        if (option_owner && decoded == "options") {
            return key.nul ? State::AmbiguousOptions : State::Options;
        }
        if ((state == State::Options || state == State::AmbiguousOptions) && decoded == "system_prompt") {
            if (key.nul || state == State::AmbiguousOptions)
                throw engine::runtime::InvalidRequestError("system_prompt key contains NUL");
            return State::Prompt;
        }
        if (state == State::Root && shape_ == SystemPromptRequestShape::NestedRequest && decoded == "request")
            return State::Request;
        if (state == State::Root && shape_ == SystemPromptRequestShape::Batch && decoded == "requests")
            return State::Requests;
        return State::Ignore;
    }

    bool value(State state) {
        space();
        if (pos_ >= text_.size()) throw engine::runtime::InvalidRequestError("invalid JSON request");
        if (text_[pos_] == '"') {
            const auto value = string_token();
            if (state == State::Prompt && value.nul)
                throw engine::runtime::InvalidRequestError("options.system_prompt cannot contain NUL");
            return state == State::Prompt;
        }
        if (text_[pos_] == '{') {
            ++pos_;
            space();
            if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return state == State::Prompt; }
            bool found_prompt = state == State::Prompt;
            bool ambiguous_options = false;
            bool normal_options_with_prompt = false;
            for (;;) {
                space();
                const auto key = string_token();
                space();
                ++pos_; // Colon: the full document has already been validated.
                const auto child_state = member(state, key);
                const bool child_prompt = value(child_state);
                found_prompt = found_prompt || child_prompt;
                ambiguous_options = ambiguous_options || child_state == State::AmbiguousOptions;
                normal_options_with_prompt = normal_options_with_prompt ||
                    (child_state == State::Options && child_prompt);
                if (ambiguous_options && normal_options_with_prompt)
                    throw engine::runtime::InvalidRequestError("system_prompt key contains NUL");
                space();
                if (text_[pos_++] == '}') return found_prompt;
            }
        }
        if (text_[pos_] == '[') {
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
        return state == State::Prompt;
    }

    std::string_view text_;
    SystemPromptRequestShape shape_;
    size_t pos_ = 0;
};

} // namespace minitts::server
