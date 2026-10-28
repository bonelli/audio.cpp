// Public legacy handlers reject every explicit slots field before touching state.
// Exercise frontend forwarding too; no test hook in the original runtime is needed.
#include "runtime.h"
#include "server_protocol.h"
#include "../../app/common/base64.h"
#include "engine/framework/io/json.h"
#include "engine/framework/debug/trace.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        minitts::server::ServerConfig config;
        config.backend = engine::core::BackendType::Cpu;
        config.ui_management = true;
        minitts::server::ServerState state(config, std::filesystem::current_path());
        minitts::server::HttpRequest ready;
        ready.method = "GET";
        ready.path = "/ready";
        for (bool forwarded : {false, true}) {
            const auto result = forwarded ? state.forward_to_core(ready) : state.handle(ready);
            if (result.status != 503) {
                throw std::runtime_error("empty legacy server reported ready");
            }
        }
        for (const std::string value : {"1", "2", "null", "false", "\"4\""}) {
            minitts::server::HttpRequest request;
            request.method = "POST";
            request.path = "/v1/models/load";
            request.body = "{\"slots\":" + value + "}";
            for (bool forwarded : {false, true}) {
                const auto response = forwarded ? state.forward_to_core(request) : state.handle(request);
                if (response.status != 400 ||
                    response.body.find("slots requires --parallel-jobs") == std::string::npos) {
                    throw std::runtime_error("legacy registration did not reject explicit slots");
                }
            }
        }
        minitts::server::HttpRequest list;
        list.method = "GET";
        list.path = "/v1/models";
        const auto response = state.handle(list);
        if (response.status != 200 ||
            !engine::io::json::parse(response.body).require("data").as_array().empty() ||
            response.body.find("slots") != std::string::npos) {
            throw std::runtime_error("rejected registration changed legacy model state");
        }
        std::cout << "legacy registration and frontend forwarding boundary passed\n";

        for (bool forwarded : {false, true}) {
            minitts::server::HttpRequest recipes;
            recipes.method = "GET";
            recipes.path = "/v1/recipes";
            const auto listed = forwarded ? state.forward_to_core(recipes) : state.handle(recipes);
            if (listed.status != 200 ||
                engine::io::json::parse(listed.body).require("data").as_array().empty()) {
                throw std::runtime_error("recipe listing was not dispatched");
            }
            recipes.method = "POST";
            recipes.path = "/v1/recipes/run";
            recipes.body = "{\"recipe\":\"unknown-recipe\"}";
            const auto rejected = forwarded ? state.forward_to_core(recipes) : state.handle(recipes);
            if (rejected.status != 400 || rejected.body.find("unknown recipe") == std::string::npos) {
                throw std::runtime_error("recipe errors bypassed shared HTTP handling");
            }
        }

        // Live speech needs `input` only for a model that speaks it. The entries
        // are lazy and the streams are never run, so no model loads and the
        // body is never read.
        minitts::server::ServerConfig live_config;
        live_config.backend = engine::core::BackendType::Cpu;
        live_config.device = 3;
        live_config.ui_enabled = false;
        for (const std::string task : {"tts", "s2s", "vc"}) {
            minitts::server::ServerModelConfig model;
            model.id = task;
            model.path = std::filesystem::temp_directory_path() / ("audiocpp-live-input-" + task);
            model.family = "live_input_fixture";
            model.task = task;
            model.mode = "streaming";
            model.lazy = true;
            if (task == "vc") model.device = 7;
            live_config.models.push_back(model);
        }
        minitts::server::ServerState live(live_config, std::filesystem::current_path());
        if (live.handle(ready).status != 503) {
            throw std::runtime_error("unloaded legacy models reported ready");
        }
        minitts::server::HttpRequest live_list;
        live_list.method = "GET";
        live_list.path = "/v1/models";
        const auto live_models = engine::io::json::parse(live.handle(live_list).body).require("data").as_array();
        if (live_models.size() != 3 || live_models[0].require("device").as_number() != 3 ||
            live_models[1].require("device").as_number() != 3 ||
            live_models[2].require("device").as_number() != 7) {
            throw std::runtime_error("legacy model listing did not resolve per-model devices");
        }
        for (const std::string task : {"tts", "s2s", "vc"}) {
            for (const std::string input : {"", "&input="}) {
                std::istream pcm(nullptr);
                minitts::server::HttpRequest request;
                request.method = "POST";
                request.path = "/v1/audio/speech/live";
                request.query = "model=" + task + input;
                request.body_stream = &pcm;
                const auto response = live.handle(request);
                const bool rejected = response.status == 400 &&
                    response.body.find("live speech requires an 'input' query parameter") != std::string::npos;
                if (rejected != (task == "tts") || (!rejected && (response.status != 200 || !response.stream_body))) {
                    throw std::runtime_error("legacy live speech without input: " + task + " got HTTP " +
                                             std::to_string(response.status) + ": " + response.body);
                }
            }
        }
        std::cout << "legacy live speech input requirement passed\n";

        // The generic routes read a request `artifacts` array before the model
        // loads. A malformed one is a 400 naming the entry; a valid one gets as
        // far as loading the model, whose path does not exist.
        minitts::server::ServerConfig task_config;
        task_config.backend = engine::core::BackendType::Cpu;
        task_config.ui_enabled = false;
        task_config.log_request_body = true;
        minitts::server::ServerModelConfig task_model;
        task_model.id = "task";
        task_model.path = std::filesystem::temp_directory_path() / "audiocpp-legacy-task";
        task_model.family = "fish_drama3";
        task_model.task = "tts";
        task_model.lazy = true;
        task_config.models.push_back(task_model);
        minitts::server::ServerState tasks(task_config, std::filesystem::current_path());
        const auto routes = [](const std::string & artifacts) {
            return std::vector<std::pair<std::string, std::string>>{
                {"/v1/tasks/run", "{\"model\":\"task\",\"text\":\"hi\",\"artifacts\":" + artifacts + "}"},
                {"/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"text\":\"hi\",\"artifacts\":" + artifacts + "}}"},
                {"/v1/tasks/stream", "{\"model\":\"task\",\"request\":{\"artifacts\":" + artifacts + "}}"},
                {"/v1/tasks/stream",
                 "{\"model\":\"task\",\"stream_format\":\"sse\",\"request\":{\"artifacts\":" + artifacts + "}}"},
                {"/v1/tasks/batch", "{\"model\":\"task\",\"requests\":[{\"text\":\"hi\"},{\"artifacts\":" + artifacts + "}]}"}};
        };
        const auto post = [&](const std::string & path, const std::string & body) {
            minitts::server::HttpRequest request;
            request.method = "POST";
            request.path = path;
            request.body = body;
            request.headers["content-type"] = "application/json";
            return tasks.handle(request);
        };
        // Serial task handlers parse inline WAV before trying the lazy model.
        constexpr size_t sample_count = 5 * 1024 * 1024 / 2 + 1;
        const auto wav = minitts::server::encode_pcm16_wav(
            engine::runtime::AudioBuffer{44100, 1, std::vector<float>(sample_count, 0.1f)});
        const auto encoded = minitts::app::base64_encode(wav);
        if (wav.size() <= 5 * 1024 * 1024) throw std::runtime_error("inline fixture is too small");
        std::string inline_outcome;
        try {
            const auto response = post("/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"audio_base64\":\"" +
                encoded + "\",\"options\":{\"encode_reference_only\":true,\"reference_text\":\"example\"}}}");
            inline_outcome = response.body;
        } catch (const std::exception & e) { inline_outcome = e.what(); }
        if (inline_outcome.find("model path does not exist") == std::string::npos)
            throw std::runtime_error("serial inline WAV did not reach the lazy model");
        for (const std::string bad : {"%%%%", "Z2FyYmFnZQ=="}) {
            const auto response = post("/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"audio_base64\":\"" + bad + "\"}}");
            if (response.status != 400 || response.body.find(bad) != std::string::npos)
                throw std::runtime_error("serial inline WAV error did not reject without echoing payload");
        }
        const auto ambiguous = post("/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"audio_base64\":\"AAAA\",\"voice_ref\":\"private-missing.wav\",\"options\":{\"encode_reference_only\":true}}}");
        if (ambiguous.status != 400 || ambiguous.body.find("only one") == std::string::npos ||
            ambiguous.body.find("private-missing.wav") != std::string::npos || ambiguous.body.find("AAAA") != std::string::npos)
            throw std::runtime_error("serial handler did not reject ambiguous reference before opening it");
        const auto invalid_option = post("/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"audio_base64\":\"AAAA\",\"voice_ref\":\"private-missing.wav\",\"options\":{\"encode_reference_only\":\"SENSITIVE_BAD_BOOLEAN\"}}}");
        if (invalid_option.status != 400 || invalid_option.body.find("encode_reference_only") == std::string::npos ||
            invalid_option.body.find("private-missing.wav") != std::string::npos ||
            invalid_option.body.find("SENSITIVE_BAD_BOOLEAN") != std::string::npos)
            throw std::runtime_error("serial handler leaked or accepted invalid encode-only option");
        for (const auto & value : {"null", "123", "false", "{\"private\":\"SENSITIVE_BAD_VALUE\"}", "[\"SENSITIVE_BAD_VALUE\"]"}) {
            for (const auto & [path, body] : std::vector<std::pair<std::string, std::string>>{
                {"/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"text\":\"hi\",\"options\":{\"system_prompt\":" + std::string(value) + "}}}"},
                {"/v1/tasks/stream", "{\"model\":\"task\",\"request\":{\"text\":\"hi\",\"options\":{\"system_prompt\":" + std::string(value) + "}}}"},
                {"/v1/tasks/batch", "{\"model\":\"task\",\"requests\":[{\"text\":\"hi\",\"options\":{\"system_prompt\":" + std::string(value) + "}}]}"},
                {"/v1/audio/speech", "{\"model\":\"task\",\"input\":\"hi\",\"options\":{\"system_prompt\":" + std::string(value) + "}}"}}) {
                const auto result = post(path, body);
                if (result.status != 400 || result.body.find("options.system_prompt must be a string") == std::string::npos ||
                    result.body.find("SENSITIVE_BAD_VALUE") != std::string::npos)
                    throw std::runtime_error("serial system_prompt type rejection failed on " + path);
            }
        }
        for (const auto & [path, body] : std::vector<std::pair<std::string, std::string>>{
            {"/v1/tasks/run", R"({"model":"task","request":{"options":{"system_prompt":"SENSITIVE\u0000TRUNCATED"}}})"},
            {"/v1/tasks/stream", R"({"model":"task","request":{"options":{"system_\u0070rompt":"SENSITIVE\u0000TRUNCATED"}}})"},
            {"/v1/tasks/batch", R"({"model":"task","requests":[{"options":{"system_prompt":"SENSITIVE\u0000TRUNCATED"}}]})"},
            {"/v1/audio/speech", R"({"model":"task","input":"hi","options":{"system_prompt":"SENSITIVE\u0000TRUNCATED"}})"}}) {
            const auto result = post(path, body);
            if (result.status != 400 || result.body.find("cannot contain NUL") == std::string::npos ||
                result.body.find("SENSITIVE") != std::string::npos)
                throw std::runtime_error("serial system_prompt NUL rejection failed on " + path);
        }
        for (const std::string path : {"/v1/tasks/run", "/v1/tasks/stream", "/v1/tasks/batch", "/v1/audio/speech"}) {
            std::string error;
            try {
                const auto result = post(path, "{\"options\":{\"system_prompt\":\"SENSITIVE_DUP_KEY\",\"SENSITIVE_DUP_KEY\":1,\"SENSITIVE_DUP_KEY\":2}}");
                if (result.status != 500) throw std::runtime_error("unexpected malformed JSON status");
                error = result.body;
            } catch (const std::runtime_error & ex) { error = ex.what(); }
            if (error.find("duplicate json object key") == std::string::npos || error.find("SENSITIVE_DUP_KEY") != std::string::npos)
                throw std::runtime_error("serial invalid JSON exposed private content on " + path);
        }
        {
            const auto log = std::filesystem::temp_directory_path() / "audiocpp-serial-reference-logging-test.log";
            std::filesystem::remove(log);
            engine::debug::configure_logging({true, log.string()});
            for (const auto & [path, body] : std::vector<std::pair<std::string, std::string>>{
                 {"/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"options\":{\"references\":\"SENSITIVE_VQ\"}}}"},
                 {"/v1/tasks/run", "{\"model\":\"task\",\"request\":{\"options\":{\"system_prompt\":\"SENSITIVE_PROMPT\"}}}"},
                 {"/v1/tasks/stream", "{\"model\":\"task\",\"request\":{\"options\":{\"system_\\u0070rompt\":\"SENSITIVE_ESCAPED_PROMPT\"}}}"},
                 {"/v1/audio/speech", "{\"model\":\"task\",\"input\":\"hello\",\"options\":{\"system_prompt\":\"SENSITIVE_BROKEN_PROMPT\""},
                 {"/not-a-route", "{\"options\":{\"system_prompt\":\"SENSITIVE_WRONG_ROUTE\"}}"},
                 {"/not-a-route", "{\"options\":{\"system_\\u0070rompt\":\"SENSITIVE_WRONG_ESCAPED\"}}"},
                 {"/not-a-route", "{\"system_\\u0070rompt\":\"SENSITIVE_WRONG_MALFORMED\""},
                 {"/not-a-route", "{\"text\":\"ORDINARY_WRONG_ROUTE\"}"},
                 {"/v1/audio/speech", "{\"model\":\"task\",\"voice_\\u0072ef\":\"SENSITIVE_AUDIO\"}"},
                 {"/v1/tasks/run", "{\"artifacts\":\"SENSITIVE_ARTIFACT\""},
                 {"/v1/tasks/run", "{\"model\":\"task\",\"text\":\"ORDINARY_SERIAL\"}"}}) {
                try { (void)post(path, body); } catch (const std::exception &) {}
            }
            std::istream live_body(nullptr);
            minitts::server::HttpRequest live_request;
            live_request.method = "POST";
            live_request.path = "/v1/audio/speech/live";
            live_request.query = "system_prompt=SENSITIVE_QUERY&foo=bar";
            live_request.body_stream = &live_body;
            minitts::server::log_request_body_if_enabled(task_config, live_request);
            engine::debug::reset_logging();
            std::ifstream input(log);
            const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            std::filesystem::remove(log);
            if (content.find("SENSITIVE_") != std::string::npos || content.find("ORDINARY_SERIAL") == std::string::npos ||
                content.find("ORDINARY_WRONG_ROUTE") == std::string::npos)
                throw std::runtime_error("serial handler logged reference data or skipped an ordinary request");
        }
        for (const auto & [bad, message] : std::vector<std::pair<std::string, std::string>>{
                 {"{}", "artifacts must be an array of artifact objects"},
                 {"[{\"id\":\"x\",\"kind\":\"tokens\",\"payload\":\"\"}]", "artifacts[0] (x): unknown kind 'tokens'"},
                 {"[{\"id\":\"x\",\"kind\":\"custom\"}]", "artifacts[0] (x): give exactly one of payload"}}) {
            for (const auto & [path, body] : routes(bad)) {
                const auto response = post(path, body);
                if (response.status != 400 || response.body.find(message) == std::string::npos ||
                    response.body.find("invalid_request_error") == std::string::npos) {
                    throw std::runtime_error("legacy " + path + " did not reject artifacts " + bad + ": HTTP " +
                                             std::to_string(response.status) + ": " + response.body);
                }
            }
        }
        const std::string good =
            "[{\"id\":\"x\",\"kind\":\"custom\",\"payload\":\"aGVsbG8=\",\"meta\":{\"n\":1}},"
            "{\"id\":\"y\",\"kind\":\"acoustic_tokens\",\"payload\":\"\"}]";
        // With stream_format sse the headers go out before the model loads, so
        // the load failure comes from the stream body, before it writes anything.
        struct Writer final : minitts::server::HttpStreamWriter {
            std::string output;
            void write(std::string_view data) override { output.append(data); }
        };
        int streamed = 0;
        for (const auto & [path, body] : routes(good)) {
            std::string outcome;
            Writer writer;
            try {
                const auto response = post(path, body);
                outcome = "HTTP " + std::to_string(response.status) + ": " + response.body;
                if (response.stream_body) {
                    ++streamed;
                    if (response.status != 200 || response.content_type != "text/event-stream; charset=utf-8" ||
                        !response.body.empty()) {
                        throw std::runtime_error("unexpected stream response " + outcome);
                    }
                    response.stream_body(writer);
                }
            } catch (const std::exception & error) {
                outcome = error.what();
            }
            if (outcome.find("model path does not exist") == std::string::npos || !writer.output.empty()) {
                throw std::runtime_error("legacy " + path + " did not take valid artifacts to the model load: " + outcome +
                                         writer.output);
            }
        }
        if (streamed != 1) {
            throw std::runtime_error("legacy /v1/tasks/stream with stream_format sse did not stream");
        }
        std::cout << "legacy request artifacts parsing passed\n";

        for (const std::string value : {"\"audio\"", "\"SSE\"", "1", "[]"}) {
            const auto response = post("/v1/tasks/stream", "{\"model\":\"task\",\"stream_format\":" + value + "}");
            if (response.status != 400 || response.stream_body ||
                response.body.find("task stream stream_format must be sse") == std::string::npos ||
                response.body.find("invalid_request_error") == std::string::npos) {
                throw std::runtime_error("legacy /v1/tasks/stream took stream_format " + value + ": HTTP " +
                                         std::to_string(response.status) + ": " + response.body);
            }
        }
        std::cout << "legacy task stream format passed\n";
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
