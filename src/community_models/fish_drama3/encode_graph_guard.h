#pragma once

namespace engine::models::fish_drama3::detail {
template<class Codec> struct EncodeGraphGuard {
    Codec & codec;
    ~EncodeGraphGuard() { codec.release_encode_graph(); }
};
}
