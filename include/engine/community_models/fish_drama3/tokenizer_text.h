#pragma once

#include "engine/framework/assets/resource_bundle.h"
#include "engine/framework/tokenizers/llama_bpe.h"

namespace engine::models::fish_drama3 {

class Drama3TextTokenizer {
public:
    explicit Drama3TextTokenizer(const assets::ResourceBundle & resources);

    std::vector<int32_t> encode(const std::string & text) const;
    bool is_control_token_id(int32_t token_id) const;

private:
    tokenizers::LlamaBpeTokenizer tokenizer_;
};

}  // namespace engine::models::fish_drama3
