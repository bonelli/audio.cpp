#include "engine/community_models/fish_drama3/tokenizer_text.h"
#include "engine/community_models/fish_drama3/prompt.h"
#include "engine/framework/io/json.h"
#include "engine/framework/runtime/errors.h"

#include <iostream>
#include <stdexcept>

namespace drama = engine::models::fish_drama3;
namespace json = engine::io::json;

void prompt_contract(const drama::Drama3TextTokenizer & tokenizer) {
    drama::Drama3Assets assets;
    drama::Drama3Take reference{"Reference.", {}};
    reference.codes.codebooks = 10;
    reference.codes.frames = 1;
    reference.codes.codes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    drama::Drama3Take previous{"Earlier.", reference.codes};
    const std::vector<drama::Drama3Take> references{reference};
    const std::vector<drama::Drama3Take> history{previous};
    drama::Drama3Take second{"Another.", reference.codes};
    second.codes.codes[0] = 11;
    const auto expected = [&](const std::string & instruction, const std::vector<drama::Drama3Take> & refs,
                              const std::vector<drama::Drama3Take> & prior) {
        std::vector<drama::Drama3Frame> rows;
        const auto append = [&](const std::string & text) {
            for (auto token : tokenizer.encode(text)) {
                drama::Drama3Frame row{};
                row[0] = token;
                rows.push_back(row);
            }
        };
        const auto append_audio = [&](const drama::Drama3Take & take) {
            drama::Drama3Frame row{};
            for (size_t i = 0; i < 10; ++i) row[i + 1] = take.codes.codes[i];
            row[0] = assets.tower.semantic_begin + take.codes.codes[0];
            rows.push_back(row);
        };
        append("<|im_start|>system\n");
        append(instruction);
        for (size_t i = 0; i < refs.size(); ++i) {
            append("\n\n[Reference " + std::to_string(i + 1) + "]\nText: <|speaker:0|> " + refs[i].text + "\nSpeech: ");
            append_audio(refs[i]);
        }
        append("<|im_end|>\n");
        for (const auto & take : prior) {
            append("<|im_start|>user\n");
            append("<|speaker:0|> " + take.text);
            append("<|im_end|>\n");
            append("<|im_start|>assistant\n<|voice|>");
            append_audio(take);
            append("<|im_end|>\n");
        }
        append("<|im_start|>user\n");
        append("<|speaker:0|> Hello.");
        append("<|im_end|>\n");
        append("<|im_start|>assistant\n<|voice|>");
        return rows;
    };
    const auto without = drama::build_drama3_prompt(assets, tokenizer, "Hello.", references, history);
    if (without != expected("convert the provided text to speech reference to the following:", references, history))
        throw std::runtime_error("default prompt changed");
    const auto custom = drama::build_drama3_prompt(assets, tokenizer, "Hello.", references, history, "Speak gently.");
    if (custom != expected("Speak gently.", references, history)) throw std::runtime_error("custom prompt framing changed");
    const auto empty = drama::build_drama3_prompt(assets, tokenizer, "Hello.", references, history, "");
    if (empty != expected("", references, history)) throw std::runtime_error("empty prompt did not replace default");
    if (custom == without || empty == without) throw std::runtime_error("system overrides were ignored");
    if (drama::build_drama3_prompt(assets, tokenizer, "Hello.", references, history) != without)
        throw std::runtime_error("system override leaked into next request");
    if (drama::build_drama3_prompt(assets, tokenizer, "Hello.", {}, {}) !=
        expected("convert the provided text to speech", {}, {}))
        throw std::runtime_error("unreferenced default prompt changed");
    const std::vector<drama::Drama3Take> two_references{reference, second};
    const std::vector<drama::Drama3Take> two_history{second, previous};
    if (drama::build_drama3_prompt(assets, tokenizer, "Hello.", two_references, two_history, "Speak gently.") !=
        expected("Speak gently.", two_references, two_history))
        throw std::runtime_error("reference or history order changed");
}

void system_prompt_validation() {
    drama::validate_drama3_system_prompt("");
    drama::validate_drama3_system_prompt("Parole: caf\xc3\xa9 [gentle] <slow>text</slow>");
    const std::vector<std::string> invalid = {
        "<|im_start|>", "<|im_end|>", "<|voice|>", "<|speaker:0|>",
        "<|audio_separator|>", "<|fim_prefix|>", "<|endofturn|>", "<|unknown|>",
        std::string("\xc0\xaf", 2), std::string("\xe2\x82", 2)
    };
    for (const auto & text : invalid) {
        bool rejected = false;
        try { drama::validate_drama3_system_prompt(text); }
        catch (const engine::runtime::InvalidRequestError & error) {
            rejected = true;
            if (std::string(error.what()).find(text) != std::string::npos)
                throw std::runtime_error("validation echoed private input");
        }
        if (!rejected) throw std::runtime_error("invalid system prompt was accepted");
    }
    drama::validate_drama3_text("<|speaker:0|> Hello [laughs]", "input");
}

int main(int argc, char ** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: tokenizer_test SIDECAR_DIRECTORY VECTORS_JSON");
        engine::assets::ResourceBundle resources(argv[1]);
        resources.add_model_files({{"tokenizer_json", "tokenizer.json"},
                                   {"tokenizer_config", "tokenizer_config.json"}});
        drama::Drama3TextTokenizer tokenizer(resources);
        system_prompt_validation();
        prompt_contract(tokenizer);
        const auto vectors = json::parse_file(argv[2]);
        int passed = 0;
        for (const auto & item : vectors.require("cases").as_array()) {
            auto text = item.require("text").as_string();
            if (const auto * bytes = item.find("utf8")) {
                text.clear();
                for (const auto & byte : bytes->as_array()) text.push_back(static_cast<char>(byte.as_i64()));
            }
            const auto tokens = tokenizer.encode(text);
            const auto & expected = item.require("tokens").as_array();
            bool equal = tokens.size() == expected.size();
            for (size_t i = 0; equal && i < tokens.size(); ++i) equal = tokens[i] == expected[i].as_i64();
            if (!equal) throw std::runtime_error("token mismatch: " + item.require("id").as_string());
            ++passed;
        }
        const std::vector<std::string> invalid = {
            std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
            std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2),
            std::string("\x80", 1),
        };
        for (const auto & text : invalid) {
            bool rejected = false;
            try { tokenizer.encode(text); }
            catch (const engine::runtime::InvalidRequestError &) { rejected = true; }
            if (!rejected) throw std::runtime_error("malformed UTF-8 accepted");
        }
        // A rejected input must not poison the persistent tokenizer.
        const auto & first = vectors.require("cases").as_array().front();
        const auto recovered = tokenizer.encode(first.require("text").as_string());
        const auto & expected = first.require("tokens").as_array();
        bool recovered_exact = recovered.size() == expected.size();
        for (size_t i = 0; recovered_exact && i < recovered.size(); ++i) {
            recovered_exact = recovered[i] == expected[i].as_i64();
        }
        if (!recovered_exact) {
            throw std::runtime_error("tokenizer recovery failed");
        }
        std::cout << "reference_vectors_exact=" << passed
                  << " malformed_utf8_rejected=" << invalid.size() << " recovery=true\n";
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
