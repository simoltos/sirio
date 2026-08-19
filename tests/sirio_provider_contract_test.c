/* Deterministic request-building tests for the Sirio DeepSeek bridge. */
#define SIRIO_NO_MAIN
#include "../sirio_provider.c"

static int test_failures;

#define TEST_ASSERT(cond) do {                                          \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: assertion failed: %s\n",            \
                    __FILE__, __LINE__, #cond);                         \
            test_failures++;                                           \
        }                                                               \
    } while (0)

typedef struct {
    bridge_buffer text;
    bridge_buffer reasoning;
    bridge_buffer provider_state;
    char request_id[64];
    char model[64];
    char finish_reason[64];
    char tool_call_id[64];
    char tool_name[64];
    char tool_arguments_json[256];
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    uint64_t total_tokens;
    uint64_t reasoning_tokens;
    uint64_t cache_hit_tokens;
    uint64_t cache_miss_tokens;
    int usage_events;
    int tool_events;
    int provider_state_events;
    int done_events;
    bool reject_text;
} stream_capture;

static void test_copy_text(char *destination, size_t capacity,
                           const char *source) {
    if (!capacity) return;
    size_t length = source ? strlen(source) : 0;
    if (length >= capacity) length = capacity - 1;
    if (length) memcpy(destination, source, length);
    destination[length] = '\0';
}

static int capture_stream_event(const sirio_bridge_event *event,
                                void *private_data) {
    stream_capture *capture = private_data;
    if (event->type == SIRIO_BRIDGE_EVENT_REASONING && event->text) {
        TEST_ASSERT(buffer_append(&capture->reasoning, event->text,
                                  strlen(event->text)) == 0);
    } else if (event->type == SIRIO_BRIDGE_EVENT_TEXT && event->text) {
        TEST_ASSERT(buffer_append(&capture->text, event->text,
                                  strlen(event->text)) == 0);
        if (capture->reject_text) return 1;
    } else if (event->type == SIRIO_BRIDGE_EVENT_TOOL_CALL) {
        capture->tool_events++;
        test_copy_text(capture->tool_call_id, sizeof(capture->tool_call_id),
                       event->tool_call_id);
        test_copy_text(capture->tool_name, sizeof(capture->tool_name),
                       event->tool_name);
        test_copy_text(capture->tool_arguments_json,
                       sizeof(capture->tool_arguments_json),
                       event->tool_arguments_json);
    } else if (event->type == SIRIO_BRIDGE_EVENT_PROVIDER_STATE) {
        capture->provider_state_events++;
        TEST_ASSERT(buffer_append(&capture->provider_state, event->text,
                                  strlen(event->text)) == 0);
    } else if (event->type == SIRIO_BRIDGE_EVENT_USAGE) {
        capture->usage_events++;
        capture->prompt_tokens = event->prompt_tokens;
        capture->completion_tokens = event->completion_tokens;
        capture->total_tokens = event->total_tokens;
        capture->reasoning_tokens = event->reasoning_tokens;
        capture->cache_hit_tokens = event->prompt_cache_hit_tokens;
        capture->cache_miss_tokens = event->prompt_cache_miss_tokens;
    } else if (event->type == SIRIO_BRIDGE_EVENT_DONE) {
        capture->done_events++;
        test_copy_text(capture->request_id, sizeof(capture->request_id),
                       event->request_id);
        test_copy_text(capture->model, sizeof(capture->model), event->model);
        test_copy_text(capture->finish_reason,
                       sizeof(capture->finish_reason),
                       event->finish_reason);
    }
    return 0;
}

static void stream_capture_free(stream_capture *capture) {
    buffer_free(&capture->text);
    buffer_free(&capture->reasoning);
    buffer_free(&capture->provider_state);
}

static void test_stream_set_sse_header(stream_response *response) {
    char header[] = "Content-Type: text/event-stream; charset=utf-8\r\n";
    TEST_ASSERT(stream_http_header(header, 1, strlen(header), response) ==
                strlen(header));
    TEST_ASSERT(response->content_type_seen);
    TEST_ASSERT(response->is_sse);
}

static int test_stream_feed_chunks(stream_response *response,
                                   const char *body, size_t stride) {
    size_t length = strlen(body);
    for (size_t offset = 0; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > stride) chunk = stride;
        if (stream_http_write((char *)body + offset, 1, chunk, response) !=
            chunk)
            return -1;
        offset += chunk;
    }
    return 0;
}

static void test_non_thinking_sampling_request(void) {
    sirio_bridge bridge = {
        .provider_id = SIRIO_PROVIDER_DEEPSEEK,
        .model = (char *)DEEPSEEK_MODEL,
    };
    sirio_generation_options options = {
        .max_tokens = 1234,
        .temperature = 0.25,
        .top_p = 0.75,
        .reasoning = SIRIO_REASONING_NONE,
    };
    TEST_ASSERT(sirio_bridge_set_generation_options(&bridge, &options) == 0);

    sirio_message message = {
        .role = SIRIO_ROLE_USER,
        .content = "hello",
    };
    bridge_buffer request = {0};
    TEST_ASSERT(build_request(&request, &bridge.generation,
                              &message, 1, NULL, 0) == 0);
    TEST_ASSERT(strstr(request.data, "\"max_tokens\":1234") != NULL);
    TEST_ASSERT(strstr(request.data,
                       "\"thinking\":{\"type\":\"disabled\"}") != NULL);
    TEST_ASSERT(strstr(request.data, "\"temperature\":0.25") != NULL);
    TEST_ASSERT(strstr(request.data, "\"top_p\":0.75") != NULL);
    TEST_ASSERT(strstr(request.data, "\"stream\":true") != NULL);
    TEST_ASSERT(strstr(request.data,
                       "\"stream_options\":{\"include_usage\":true}") != NULL);
    buffer_free(&request);
}

static void test_thinking_request(void) {
    sirio_bridge bridge = {
        .provider_id = SIRIO_PROVIDER_DEEPSEEK,
        .model = (char *)DEEPSEEK_MODEL,
    };
    sirio_generation_options options = {
        .max_tokens = 50000,
        .temperature = -1.0,
        .top_p = -1.0,
        .reasoning = SIRIO_REASONING_MAX,
    };
    TEST_ASSERT(sirio_bridge_set_generation_options(&bridge, &options) == 0);

    sirio_message message = {
        .role = SIRIO_ROLE_USER,
        .content = "hello",
    };
    bridge_buffer request = {0};
    TEST_ASSERT(build_request(&request, &bridge.generation,
                              &message, 1, NULL, 0) == 0);
    TEST_ASSERT(strstr(request.data,
                       "\"thinking\":{\"type\":\"enabled\"}") != NULL);
    TEST_ASSERT(strstr(request.data, "\"reasoning_effort\":\"max\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"temperature\"") == NULL);
    TEST_ASSERT(strstr(request.data, "\"top_p\"") == NULL);
    buffer_free(&request);
}

static void test_structured_tool_request_remains_optional(void) {
    sirio_generation_options options = {
        .temperature = -1.0,
        .top_p = -1.0,
        .reasoning = SIRIO_REASONING_NONE,
    };
    sirio_message message = {
        .role = SIRIO_ROLE_USER,
        .content = "hello",
    };
    sirio_tool tool = {
        .name = "read",
        .description = "Read a file",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
    };
    bridge_buffer request = {0};
    TEST_ASSERT(build_request(&request, &options,
                              &message, 1, &tool, 1) == 0);
    TEST_ASSERT(strstr(request.data, "\"tools\":[") != NULL);
    TEST_ASSERT(strstr(request.data, "\"name\":\"read\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"tool_choice\":\"auto\"") != NULL);
    buffer_free(&request);

    options.reasoning = SIRIO_REASONING_HIGH;
    TEST_ASSERT(build_request(&request, &options,
                              &message, 1, &tool, 1) == 0);
    TEST_ASSERT(strstr(request.data, "\"tools\":[") != NULL);
    TEST_ASSERT(strstr(request.data, "\"tool_choice\"") == NULL);
    buffer_free(&request);
}

static void test_normalized_tool_exchange_maps_to_deepseek_wire(void) {
    sirio_generation_options options = {
        .temperature = -1.0,
        .top_p = -1.0,
        .reasoning = SIRIO_REASONING_HIGH,
    };
    sirio_tool_call call = {
        .id = "call_1",
        .name = "read",
        .arguments_json = "{\"path\":\"x.c\"}",
    };
    sirio_message messages[] = {
        {.role = SIRIO_ROLE_SYSTEM, .content = "system"},
        {.role = SIRIO_ROLE_USER, .content = "inspect"},
        {
            .role = SIRIO_ROLE_ASSISTANT,
            .content = "",
            .reasoning = "need the file",
            .tool_calls = &call,
            .tool_call_count = 1,
        },
        {
            .role = SIRIO_ROLE_TOOL,
            .content = "contents",
            .tool_call_id = "call_1",
        },
    };
    sirio_tool tool = {
        .name = "read",
        .description = "Read a file",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
    };
    bridge_buffer request = {0};
    TEST_ASSERT(build_request(&request, &options, messages,
                              sizeof(messages) / sizeof(messages[0]),
                              &tool, 1) == 0);
    TEST_ASSERT(strstr(request.data,
                       "\"reasoning_content\":\"need the file\"") != NULL);
    TEST_ASSERT(strstr(request.data,
                       "\"tool_calls\":[{\"id\":\"call_1\"") != NULL);
    TEST_ASSERT(strstr(request.data,
                       "\"arguments\":\"{\\\"path\\\":\\\"x.c\\\"}\"") != NULL);
    TEST_ASSERT(strstr(request.data,
                       "\"role\":\"tool\",\"content\":\"contents\",\"tool_call_id\":\"call_1\"") != NULL);
    buffer_free(&request);

    messages[2].reasoning = NULL;
    TEST_ASSERT(build_request(&request, &options, messages,
                              sizeof(messages) / sizeof(messages[0]),
                              &tool, 1) == 0);
    TEST_ASSERT(strstr(request.data, "\"reasoning_content\"") == NULL);
    TEST_ASSERT(strstr(request.data,
                       "\"tool_calls\":[{\"id\":\"call_1\"") != NULL);
    buffer_free(&request);

    TEST_ASSERT(build_request(&request, &options, messages, 3,
                              &tool, 1) != 0);
    sirio_message orphan = {
        .role = SIRIO_ROLE_TOOL,
        .content = "orphan",
        .tool_call_id = "call_1",
    };
    TEST_ASSERT(build_request(&request, &options, &orphan, 1,
                              &tool, 1) != 0);
    buffer_free(&request);
}

static void test_normalized_results_are_correlated_by_id(void) {
    sirio_generation_options options = {
        .temperature = -1.0,
        .top_p = -1.0,
        .reasoning = SIRIO_REASONING_NONE,
    };
    sirio_tool_call calls[] = {
        {.id = "call-a", .name = "read",
         .arguments_json = "{\"path\":\"a\"}"},
        {.id = "call-b", .name = "read",
         .arguments_json = "{\"path\":\"b\"}"},
    };
    sirio_message messages[] = {
        {
            .role = SIRIO_ROLE_ASSISTANT,
            .content = NULL,
            .tool_calls = calls,
            .tool_call_count = 2,
        },
        {
            .role = SIRIO_ROLE_TOOL,
            .content = "b result",
            .tool_call_id = "call-b",
        },
        {
            .role = SIRIO_ROLE_TOOL,
            .content = "a result",
            .tool_call_id = "call-a",
        },
    };
    sirio_tool tool = {
        .name = "read",
        .description = "Read a file",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
    };
    bridge_buffer request = {0};
    TEST_ASSERT(build_request(&request, &options, messages, 3,
                              &tool, 1) == 0);
    TEST_ASSERT(strstr(request.data, "\"content\":null") != NULL);
    buffer_free(&request);

    messages[2].tool_call_id = "call-b";
    TEST_ASSERT(build_request(&request, &options, messages, 3,
                              &tool, 1) != 0);
    buffer_free(&request);
}

static void test_flat_tool_argument_decoder(void) {
    sirio_tool_argument *arguments = NULL;
    size_t count = 0;
    char err[160] = {0};
    TEST_ASSERT(sirio_tool_arguments_parse(
        "{\"path\":\"a\\nb\",\"line\":12,\"whole\":false}",
        &arguments, &count, err, sizeof(err)) == 0);
    TEST_ASSERT(count == 3);
    TEST_ASSERT(arguments[0].type == SIRIO_TOOL_ARGUMENT_STRING);
    TEST_ASSERT(!strcmp(arguments[0].value, "a\nb"));
    TEST_ASSERT(arguments[1].type == SIRIO_TOOL_ARGUMENT_NUMBER);
    TEST_ASSERT(arguments[2].type == SIRIO_TOOL_ARGUMENT_BOOLEAN);
    sirio_tool_arguments_free(arguments, count);

    arguments = NULL;
    count = 0;
    TEST_ASSERT(sirio_tool_arguments_parse(
        "{\"nested\":{}}", &arguments, &count,
        err, sizeof(err)) != 0);
    TEST_ASSERT(strstr(err, "strings, numbers, or booleans") != NULL);
    TEST_ASSERT(arguments == NULL);
    TEST_ASSERT(count == 0);

    TEST_ASSERT(sirio_tool_arguments_parse(
        "{\"x\":1,\"x\":2}", &arguments, &count,
        err, sizeof(err)) != 0);
    TEST_ASSERT(strstr(err, "duplicate") != NULL);

    static const char schema[] =
        "{\"type\":\"object\",\"properties\":{\"line\":{\"type\":\"integer\"}},\"required\":[\"line\"],\"additionalProperties\":false}";
    arguments = NULL;
    count = 0;
    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{\"line\":2.0}", schema, &arguments, &count,
        err, sizeof(err)) == 0);
    TEST_ASSERT(count == 1);
    TEST_ASSERT(!strcmp(arguments[0].value, "2"));
    sirio_tool_arguments_free(arguments, count);

    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{\"line\":2,\"extra\":true}", schema,
        &arguments, &count, err, sizeof(err)) != 0);
    TEST_ASSERT(strstr(err, "unknown tool argument") != NULL);

    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{\"line\":9223372036854775807}", schema,
        &arguments, &count, err, sizeof(err)) == 0);
    TEST_ASSERT(!strcmp(arguments[0].value, "9223372036854775807"));
    sirio_tool_arguments_free(arguments, count);
    arguments = NULL;
    count = 0;
    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{\"line\":9223372036854775808}", schema,
        &arguments, &count, err, sizeof(err)) != 0);
    TEST_ASSERT(strstr(err, "portable integer") != NULL);
    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{\"line\":1e2}", schema,
        &arguments, &count, err, sizeof(err)) == 0);
    TEST_ASSERT(!strcmp(arguments[0].value, "100"));
    sirio_tool_arguments_free(arguments, count);
    arguments = NULL;
    count = 0;
    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{\"line\":1e-1}", schema,
        &arguments, &count, err, sizeof(err)) != 0);

    static const char bad_root_schema[] =
        "{\"type\":\"array\",\"properties\":{}}";
    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{}", bad_root_schema, &arguments, &count,
        err, sizeof(err)) != 0);
    static const char missing_required_schema[] =
        "{\"type\":\"object\",\"properties\":{},\"required\":[\"x\"]}";
    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{}", missing_required_schema, &arguments, &count,
        err, sizeof(err)) != 0);
    static const char unsupported_type_schema[] =
        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"array\"}}}";
    TEST_ASSERT(sirio_tool_arguments_parse_validated(
        "{}", unsupported_type_schema, &arguments, &count,
        err, sizeof(err)) != 0);
}

static void test_invalid_generation_options(void) {
    sirio_bridge bridge = {
        .provider_id = SIRIO_PROVIDER_DEEPSEEK,
        .model = (char *)DEEPSEEK_MODEL,
    };
    sirio_generation_options options = {
        .temperature = 2.1,
        .top_p = -1.0,
        .reasoning = SIRIO_REASONING_NONE,
    };
    TEST_ASSERT(sirio_bridge_set_generation_options(&bridge, &options) != 0);
    TEST_ASSERT(strstr(bridge.last_error, "temperature") != NULL);

    options.temperature = 0.5;
    options.reasoning = SIRIO_REASONING_HIGH;
    TEST_ASSERT(sirio_bridge_set_generation_options(&bridge, &options) != 0);
    TEST_ASSERT(strstr(bridge.last_error, "reasoning mode") != NULL);

    options.max_tokens = SIRIO_MODEL_MAX_OUTPUT_TOKENS + 1;
    options.temperature = -1.0;
    options.top_p = -1.0;
    options.reasoning = SIRIO_REASONING_NONE;
    TEST_ASSERT(sirio_bridge_set_generation_options(&bridge, &options) != 0);
    TEST_ASSERT(strstr(bridge.last_error, "output limit") != NULL);
}

static void test_model_catalog_capabilities(void) {
    const sirio_model_info *flash = sirio_model_find_for_provider(
        SIRIO_PROVIDER_DEEPSEEK, DEEPSEEK_MODEL);
    const sirio_model_info *go_flash = sirio_model_find_for_provider(
        SIRIO_PROVIDER_OPENCODE_GO, OPENCODE_GO_MODEL);
    const sirio_model_info *go_pro = sirio_model_find_for_provider(
        SIRIO_PROVIDER_OPENCODE_GO, DEEPSEEK_PRO_MODEL);
    const sirio_model_info *sol = sirio_model_find("gpt-5.6-sol");
    const sirio_model_info *terra = sirio_model_find("gpt-5.6-terra");
    const sirio_model_info *luna = sirio_model_find(OPENAI_MODEL);
    const sirio_model_info *go_glm = sirio_model_find_for_provider(
        SIRIO_PROVIDER_OPENCODE_GO, OPENCODE_GO_GLM_MODEL);
    const sirio_model_info *kimi_k3 = sirio_model_find_for_provider(
        SIRIO_PROVIDER_KIMI, KIMI_MODEL);
    const sirio_model_info *kimi_coding = sirio_model_find_for_provider(
        SIRIO_PROVIDER_KIMI, KIMI_CODING_MODEL);
    sirio_reasoning_effort parsed = SIRIO_REASONING_NONE;
    sirio_reasoning_effort next = SIRIO_REASONING_NONE;

    TEST_ASSERT(flash != NULL && go_flash != NULL && go_pro != NULL);
    TEST_ASSERT(sol != NULL && terra != NULL && luna != NULL);
    TEST_ASSERT(go_glm != NULL && kimi_k3 != NULL && kimi_coding != NULL);
    TEST_ASSERT(sol && sol->context_tokens == OPENAI_MODEL_CONTEXT_TOKENS);
    TEST_ASSERT(terra && terra->default_reasoning == SIRIO_REASONING_MEDIUM);
    TEST_ASSERT(luna && luna->default_reasoning == SIRIO_REASONING_LOW);
    TEST_ASSERT(flash && sirio_model_supports_reasoning(
        flash, SIRIO_REASONING_NONE));
    TEST_ASSERT(flash && sirio_model_supports_reasoning(
        flash, SIRIO_REASONING_LOW));
    TEST_ASSERT(go_flash && go_flash->default_reasoning ==
                              SIRIO_REASONING_NONE);
    TEST_ASSERT(go_flash && sirio_model_supports_reasoning(
        go_flash, SIRIO_REASONING_NONE));
    TEST_ASSERT(go_flash && sirio_model_supports_reasoning(
        go_flash, SIRIO_REASONING_LOW));
    TEST_ASSERT(go_pro && !sirio_model_supports_reasoning(
        go_pro, SIRIO_REASONING_NONE));
    TEST_ASSERT(go_pro && !sirio_model_supports_reasoning(
        go_pro, SIRIO_REASONING_LOW));
    TEST_ASSERT(sol && !sirio_model_supports_reasoning(
        sol, SIRIO_REASONING_NONE));
    TEST_ASSERT(sol && sirio_model_supports_reasoning(
        sol, SIRIO_REASONING_MAX));
    TEST_ASSERT(luna && sirio_model_supports_reasoning(
        luna, SIRIO_REASONING_MAX));
    TEST_ASSERT(go_glm && sirio_model_supports_reasoning(
        go_glm, SIRIO_REASONING_LOW));
    TEST_ASSERT(kimi_k3 && kimi_k3->context_tokens == KIMI_CONTEXT_TOKENS);
    TEST_ASSERT(kimi_k3 && sirio_model_supports_reasoning(
        kimi_k3, SIRIO_REASONING_MAX));
    TEST_ASSERT(kimi_coding && sirio_model_supports_reasoning(
        kimi_coding, SIRIO_REASONING_HIGH));
    TEST_ASSERT(kimi_coding && !sirio_model_supports_reasoning(
        kimi_coding, SIRIO_REASONING_LOW));
    TEST_ASSERT(!sirio_reasoning_parse("ultra", &parsed));
    TEST_ASSERT(sol && !sirio_model_step_reasoning(
        sol, SIRIO_REASONING_MAX, 1, &next));
}

static void test_sse_text_reasoning_usage_and_metadata(void) {
    stream_capture capture = {0};
    stream_response response = {
        .callback = capture_stream_event,
        .private_data = &capture,
    };
    test_stream_set_sse_header(&response);
    const char *body =
        "data: {\"id\":\"req-1\",\"model\":\"deepseek-chat\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"r\xc3\xa9\",\"content\":null},\"finish_reason\":null}]}\r\n\r\n"
        "data: {\"id\":\"req-1\",\"model\":\"deepseek-chat\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hello \"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"req-1\",\"model\":\"deepseek-chat\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"world\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"id\":\"req-1\",\"model\":\"deepseek-chat\",\"choices\":[],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":6,\"total_tokens\":13,\"prompt_cache_hit_tokens\":2,\"prompt_cache_miss_tokens\":5,\"completion_tokens_details\":{\"reasoning_tokens\":3}}}\n\n"
        "data: [DONE]\n\n";

    TEST_ASSERT(test_stream_feed_chunks(&response, body, 7) == 0);
    TEST_ASSERT(stream_finish(&response) == 0);
    TEST_ASSERT(capture.reasoning.data != NULL);
    TEST_ASSERT(!strcmp(capture.reasoning.data, "r\xc3\xa9"));
    TEST_ASSERT(capture.text.data != NULL);
    TEST_ASSERT(!strcmp(capture.text.data, "hello world"));
    TEST_ASSERT(capture.usage_events == 1);
    TEST_ASSERT(capture.done_events == 1);
    TEST_ASSERT(!strcmp(capture.request_id, "req-1"));
    TEST_ASSERT(!strcmp(capture.model, "deepseek-chat"));
    TEST_ASSERT(!strcmp(capture.finish_reason, "stop"));
    TEST_ASSERT(capture.prompt_tokens == 7);
    TEST_ASSERT(capture.completion_tokens == 6);
    TEST_ASSERT(capture.total_tokens == 13);
    TEST_ASSERT(capture.reasoning_tokens == 3);
    TEST_ASSERT(capture.cache_hit_tokens == 2);
    TEST_ASSERT(capture.cache_miss_tokens == 5);

    stream_response_free(&response);
    stream_capture_free(&capture);
}

static void test_sse_tool_call_fragments(void) {
    stream_capture capture = {0};
    stream_response response = {
        .callback = capture_stream_event,
        .private_data = &capture,
    };
    test_stream_set_sse_header(&response);
    const char *body =
        "data: {\"id\":\"req-tool\",\"model\":\"deepseek-chat\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"rea\",\"arguments\":\"{\\\"pa\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"req-tool\",\"model\":\"deepseek-chat\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"d\",\"arguments\":\"th\\\":\\\"x\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: {\"id\":\"req-tool\",\"model\":\"deepseek-chat\",\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4,\"total_tokens\":14}}\n\n"
        "data: [DONE]\n\n";

    TEST_ASSERT(test_stream_feed_chunks(&response, body, 11) == 0);
    TEST_ASSERT(stream_finish(&response) == 0);
    TEST_ASSERT(capture.tool_events == 1);
    TEST_ASSERT(!strcmp(capture.tool_call_id, "call_1"));
    TEST_ASSERT(!strcmp(capture.tool_name, "read"));
    TEST_ASSERT(!strcmp(capture.tool_arguments_json,
                        "{\"path\":\"x\"}"));
    TEST_ASSERT(!strcmp(capture.finish_reason, "tool_calls"));

    stream_response_free(&response);
    stream_capture_free(&capture);
}

static void test_sse_rejects_callback_and_malformed_terminal_state(void) {
    stream_capture capture = {.reject_text = true};
    stream_response rejected = {
        .callback = capture_stream_event,
        .private_data = &capture,
    };
    test_stream_set_sse_header(&rejected);
    const char *text_event =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{\"delta\":{\"content\":\"x\"},\"finish_reason\":null}]}\n\n";
    TEST_ASSERT(test_stream_feed_chunks(&rejected, text_event,
                                        strlen(text_event)) != 0);
    TEST_ASSERT(rejected.callback_rejected);
    TEST_ASSERT(strstr(rejected.error, "rejected") != NULL);
    stream_response_free(&rejected);
    stream_capture_free(&capture);

    stream_response malformed = {0};
    test_stream_set_sse_header(&malformed);
    TEST_ASSERT(test_stream_feed_chunks(&malformed,
                                        "data: {\"id\":]\n\n", 3) != 0);
    TEST_ASSERT(strstr(malformed.error, "malformed") != NULL);
    stream_response_free(&malformed);

    stream_response incomplete = {0};
    test_stream_set_sse_header(&incomplete);
    const char *without_usage =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    TEST_ASSERT(test_stream_feed_chunks(&incomplete, without_usage, 5) == 0);
    TEST_ASSERT(stream_finish(&incomplete) != 0);
    TEST_ASSERT(strstr(incomplete.error, "usage") != NULL);
    stream_response_free(&incomplete);
}

static void test_openai_pkce_sha256(void) {
    openai_sha256 context;
    unsigned char digest[32];
    static const unsigned char expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };
    openai_sha256_init(&context);
    openai_sha256_update(&context, "abc", 3);
    openai_sha256_final(&context, digest);
    TEST_ASSERT(!memcmp(digest, expected, sizeof(expected)));
}

static void test_openai_jwt_claims(void) {
    static const char payload[] =
        "{\"exp\":2000000000,\"https://api.openai.com/auth\":{"
        "\"chatgpt_account_id\":\"acct-test\"}}";
    char *encoded = openai_base64url_encode(
        (const unsigned char *)payload, strlen(payload));
    TEST_ASSERT(encoded != NULL);
    bridge_buffer token = {0};
    TEST_ASSERT(buffer_puts(&token, "e30.") == 0);
    TEST_ASSERT(buffer_puts(&token, encoded) == 0);
    TEST_ASSERT(buffer_puts(&token, ".signature") == 0);
    openai_auth auth = {.access_token = buffer_release(&token)};
    openai_auth_derive_claims(&auth);
    TEST_ASSERT(auth.access_expires_at == (time_t)2000000000);
    TEST_ASSERT(auth.account_id && !strcmp(auth.account_id, "acct-test"));
    openai_auth_free(&auth);
    free(encoded);
}

static void test_openai_auth_round_trip(void) {
    char path[] = "/tmp/sirio-openai-auth-XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0);
    if (fd < 0) return;
    close(fd);
    unlink(path);

    char store_error[256] = {0};
    sirio_auth_store *store = sirio_auth_store_load(
        path, store_error, sizeof(store_error));
    TEST_ASSERT(store != NULL);
    if (!store) return;
    TEST_ASSERT(sirio_auth_store_set_api_key(
        store, SIRIO_PROVIDER_DEEPSEEK, "deepseek-key") == 0);
    TEST_ASSERT(sirio_auth_store_set_preferred(
        store, SIRIO_PROVIDER_DEEPSEEK, SIRIO_AUTH_API_KEY) == 0);
    TEST_ASSERT(sirio_auth_store_set_api_key(
        store, SIRIO_PROVIDER_OPENAI, "openai-key") == 0);
    TEST_ASSERT(sirio_auth_store_set_preferred(
        store, SIRIO_PROVIDER_OPENAI, SIRIO_AUTH_API_KEY) == 0);
    TEST_ASSERT(sirio_auth_store_save(
        store, path, store_error, sizeof(store_error)) == 0);
    sirio_auth_store_destroy(store);

    sirio_bridge source = {
        .provider = &provider_implementations[SIRIO_PROVIDER_OPENAI],
        .provider_id = SIRIO_PROVIDER_OPENAI,
        .auth_method = SIRIO_AUTH_OAUTH,
    };
    source.openai.id_token = bridge_strdup("id-token");
    source.openai.access_token = bridge_strdup("access-token");
    source.openai.refresh_token = bridge_strdup("refresh-token");
    source.openai.account_id = bridge_strdup("acct-test");
    source.openai.access_expires_at = (time_t)2000000000;
    source.openai.refreshed_at = (time_t)1900000000;
    TEST_ASSERT(sirio_bridge_save_auth(&source, path) == 0);

    store = sirio_auth_store_load(path, store_error, sizeof(store_error));
    TEST_ASSERT(store != NULL);
    if (store) {
        TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_DEEPSEEK, SIRIO_AUTH_API_KEY));
        TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_OPENAI, SIRIO_AUTH_API_KEY));
        TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_OPENAI, SIRIO_AUTH_OAUTH));
        TEST_ASSERT(sirio_auth_store_preferred(
            store, SIRIO_PROVIDER_OPENAI) == SIRIO_AUTH_OAUTH);
        sirio_auth_store_destroy(store);
    }

    struct stat st;
    TEST_ASSERT(stat(path, &st) == 0);
    TEST_ASSERT((st.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    TEST_ASSERT((st.st_mode & (S_IRUSR | S_IWUSR)) ==
                (S_IRUSR | S_IWUSR));

    sirio_bridge loaded = {
        .provider = &provider_implementations[SIRIO_PROVIDER_OPENAI],
        .provider_id = SIRIO_PROVIDER_OPENAI,
        .auth_method = SIRIO_AUTH_OAUTH,
    };
    TEST_ASSERT(sirio_bridge_load_auth(&loaded, path) == 0);
    TEST_ASSERT(loaded.openai.id_token &&
                !strcmp(loaded.openai.id_token, "id-token"));
    TEST_ASSERT(loaded.openai.access_token &&
                !strcmp(loaded.openai.access_token, "access-token"));
    TEST_ASSERT(loaded.openai.refresh_token &&
                !strcmp(loaded.openai.refresh_token, "refresh-token"));
    TEST_ASSERT(loaded.openai.account_id &&
                !strcmp(loaded.openai.account_id, "acct-test"));
    TEST_ASSERT(loaded.openai.access_expires_at == (time_t)2000000000);
    TEST_ASSERT(loaded.openai.refreshed_at == (time_t)1900000000);

    TEST_ASSERT(chmod(path, S_IRUSR | S_IWUSR | S_IRGRP) == 0);
    sirio_bridge rejected = {
        .provider = &provider_implementations[SIRIO_PROVIDER_OPENAI],
        .provider_id = SIRIO_PROVIDER_OPENAI,
        .auth_method = SIRIO_AUTH_OAUTH,
    };
    TEST_ASSERT(sirio_bridge_load_auth(&rejected, path) != 0);
    TEST_ASSERT(strstr(rejected.last_error, "cannot read") != NULL);

    openai_auth_free(&source.openai);
    openai_auth_free(&loaded.openai);
    openai_auth_free(&rejected.openai);
    unlink(path);
}

static void test_auth_store_rejects_wrong_shape(void) {
    char path[] = "/tmp/sirio-invalid-auth-XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0);
    if (fd < 0) return;
    static const char invalid[] =
        "{\"providers\":{\"deepseek\":{},\"openai\":{}},"
        "\"unexpected\":true}\n";
    TEST_ASSERT(write(fd, invalid, sizeof(invalid) - 1) ==
                (ssize_t)(sizeof(invalid) - 1));
    TEST_ASSERT(close(fd) == 0);
    char error[256] = {0};
    sirio_auth_store *store = sirio_auth_store_load(
        path, error, sizeof(error));
    TEST_ASSERT(store == NULL);
    TEST_ASSERT(strstr(error, "invalid auth file") != NULL);
    sirio_auth_store_destroy(store);
    unlink(path);
}

static void test_auth_store_accepts_missing_providers(void) {
    char path[] = "/tmp/sirio-partial-auth-XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0);
    if (fd < 0) return;
    static const char partial[] =
        "{\"providers\":{\"deepseek\":{\"auth_method\":\"api-key\","
        "\"api_key\":\"deepseek-key\"}}}\n";
    TEST_ASSERT(write(fd, partial, sizeof(partial) - 1) ==
                (ssize_t)(sizeof(partial) - 1));
    TEST_ASSERT(close(fd) == 0);

    char error[256] = {0};
    sirio_auth_store *store = sirio_auth_store_load(
        path, error, sizeof(error));
    TEST_ASSERT(store != NULL);
    if (store) {
        TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_DEEPSEEK, SIRIO_AUTH_API_KEY));
        TEST_ASSERT(!sirio_auth_store_has(
            store, SIRIO_PROVIDER_OPENAI, SIRIO_AUTH_API_KEY));
        TEST_ASSERT(!sirio_auth_store_has(
            store, SIRIO_PROVIDER_OPENCODE_GO, SIRIO_AUTH_API_KEY));
        TEST_ASSERT(!sirio_auth_store_has(
            store, SIRIO_PROVIDER_KIMI, SIRIO_AUTH_API_KEY));
        TEST_ASSERT(sirio_auth_store_save(
            store, path, error, sizeof(error)) == 0);
        sirio_auth_store_destroy(store);
    }

    char *saved = NULL;
    size_t saved_length = 0;
    TEST_ASSERT(provider_read_auth_file(
        path, &saved, &saved_length) == 0);
    TEST_ASSERT(saved_length > 0);
    TEST_ASSERT(saved && strstr(saved, "\"deepseek\""));
    TEST_ASSERT(saved && !strstr(saved, "\"openai\""));
    TEST_ASSERT(saved && !strstr(saved, "\"opencode-go\""));
    TEST_ASSERT(saved && !strstr(saved, "\"kimi\""));
    free(saved);
    unlink(path);
}

static void test_model_store_rejects_wrong_shape(void) {
    char path[] = "/tmp/sirio-invalid-models-XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0);
    if (fd < 0) return;
    TEST_ASSERT(close(fd) == 0);
    static const char *const invalid[] = {
        "{}\n",
        "{\"deepseek\":[],\"last_used\":{}}\n",
        "{\"interface\":{\"models\":[],"
            "\"last_used\":{\"model\":null}}}\n",
        "{\"interface\":{\"models\":[\"deepseek-v4-flash\"],"
            "\"last_used\":{\"model\":null}},\"opencode-go\":[{"
            "\"id\":\"deepseek-v4-flash\",\"last_effort\":\"none\","
            "\"active\":true}]}\n",
        "{\"interface\":{\"models\":[\"opencode-go/deepseek-v4-flash\","
            "\"opencode-go/deepseek-v4-flash\"],"
            "\"last_used\":{\"model\":null}},"
            "\"opencode-go\":[{\"id\":\"deepseek-v4-flash\","
            "\"last_effort\":\"none\",\"active\":true}]}\n",
        "{\"interface\":{\"models\":[\"opencode-go/unknown\"],"
            "\"last_used\":{\"model\":null}},\"opencode-go\":[]}\n",
        "{\"interface\":{\"models\":[\"opencode-go/deepseek-v4-flash\"],"
            "\"last_used\":{\"model\":"
            "\"opencode-go/deepseek-v4-pro\"}},"
            "\"opencode-go\":[{\"id\":\"deepseek-v4-flash\","
            "\"last_effort\":\"none\",\"active\":true},{"
            "\"id\":\"deepseek-v4-pro\",\"last_effort\":\"high\","
            "\"active\":true}]}\n",
        "{\"interface\":{\"models\":[\"kimi/k3\"],"
            "\"last_used\":{\"model\":null}},"
            "\"kimi\":[{\"id\":\"k3\",\"alias\":\"k3\","
            "\"last_effort\":\"high\",\"active\":true}]}\n",
        "{\"interface\":{\"models\":[\"kimi/k3\"],"
            "\"last_used\":{\"model\":null}},"
            "\"kimi\":[{\"id\":\"k3\",\"last_effort\":\"high\","
            "\"active\":\"true\"}]}\n",
        "{\"interface\":{\"models\":[\"kimi/k3\"],"
            "\"last_used\":{\"model\":null,\"provider\":\"kimi\"}},"
            "\"kimi\":[{\"id\":\"k3\",\"last_effort\":\"high\","
            "\"active\":true}]}\n",
        "{\"interface\":{\"models\":["
            "\"opencode-go/deepseek-v4-flash\"],"
            "\"last_used\":{\"model\":null}},\"opencode-go\":[{"
            "\"id\":\"deepseek-v4-flash\","
            "\"last_effort\":\"medium\",\"active\":true}]}\n",
    };
    char error[256] = {0};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,
                  S_IRUSR | S_IWUSR);
        TEST_ASSERT(fd >= 0);
        if (fd < 0) break;
        size_t length = strlen(invalid[i]);
        TEST_ASSERT(write(fd, invalid[i], length) == (ssize_t)length);
        TEST_ASSERT(close(fd) == 0);
        sirio_model_store *store = sirio_model_store_load(
            path, error, sizeof(error));
        TEST_ASSERT(store == NULL);
        TEST_ASSERT(strstr(error, "invalid models file") != NULL);
        sirio_model_store_destroy(store);
    }
    unlink(path);

    sirio_model_store *missing = sirio_model_store_load(
        path, error, sizeof(error));
    TEST_ASSERT(missing == NULL);
    TEST_ASSERT(strstr(error, "does not exist") != NULL);
    sirio_model_store_destroy(missing);
}

static void test_model_store_uses_configured_catalog_and_interface(void) {
    char path[] = "/tmp/sirio-reconcile-models-XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0);
    if (fd < 0) return;
    static const char partial[] =
        "{\"interface\":{\"models\":[\"openai/gpt-5.6-luna\","
        "\"opencode-go/deepseek-v4-flash\","
        "\"opencode-go/deepseek-v4-pro\"],"
        "\"last_used\":{\"model\":"
        "\"opencode-go/deepseek-v4-flash\"}},"
        "\"openai\":[{\"id\":\"gpt-5.6-luna\","
        "\"last_effort\":\"low\",\"active\":true}],"
        "\"opencode-go\":[{\"id\":\"deepseek-v4-flash\","
        "\"last_effort\":\"none\",\"active\":false},{"
        "\"id\":\"deepseek-v4-pro\",\"last_effort\":\"high\","
        "\"active\":true},{\"id\":\"glm-5.3\","
        "\"last_effort\":\"high\",\"active\":true}]}\n";
    TEST_ASSERT(write(fd, partial, sizeof(partial) - 1) ==
                (ssize_t)(sizeof(partial) - 1));
    TEST_ASSERT(close(fd) == 0);

    char error[256] = {0};
    sirio_model_store *store = sirio_model_store_load(
        path, error, sizeof(error));
    TEST_ASSERT(store != NULL);
    if (store) {
        TEST_ASSERT(sirio_model_store_count(
            store, SIRIO_PROVIDER_DEEPSEEK) == 0);
        TEST_ASSERT(sirio_model_store_count(
            store, SIRIO_PROVIDER_OPENAI) == 1);
        TEST_ASSERT(sirio_model_store_count(
            store, SIRIO_PROVIDER_OPENCODE_GO) == 3);
        TEST_ASSERT(sirio_model_store_count(
            store, SIRIO_PROVIDER_KIMI) == 0);
        TEST_ASSERT(sirio_model_store_interface_count(store) == 3);

        bool active = true;
        const sirio_model_info *flash = sirio_model_store_at(
            store, SIRIO_PROVIDER_OPENCODE_GO, 0, NULL, &active);
        TEST_ASSERT(flash && !strcmp(flash->name, DEEPSEEK_MODEL));
        TEST_ASSERT(!active);
        const sirio_model_info *luna = sirio_model_store_interface_at(
            store, 0, NULL, &active);
        TEST_ASSERT(luna && !strcmp(luna->name, OPENAI_MODEL));
        TEST_ASSERT(active);
        const sirio_model_info *first = sirio_model_store_first_active(
            store, SIRIO_PROVIDER_OPENCODE_GO, NULL);
        TEST_ASSERT(first && !strcmp(first->name, DEEPSEEK_PRO_MODEL));
        TEST_ASSERT(sirio_model_store_resolve(
            store, SIRIO_PROVIDER_OPENCODE_GO, DEEPSEEK_MODEL,
            error, sizeof(error)) == NULL);
        TEST_ASSERT(strstr(error, "inactive") != NULL);
        const sirio_model_info *last = NULL;
        TEST_ASSERT(!sirio_model_store_last_used(store, &last));
        TEST_ASSERT(sirio_model_store_set_last_used(
            store, flash, SIRIO_REASONING_NONE) != 0);
        const sirio_model_info *glm = sirio_model_store_resolve(
            store, SIRIO_PROVIDER_OPENCODE_GO, OPENCODE_GO_GLM_MODEL,
            error, sizeof(error));
        TEST_ASSERT(glm != NULL);
        TEST_ASSERT(sirio_model_store_resolve_interface(
            store, "opencode-go/glm-5.3", error, sizeof(error)) == NULL);
        TEST_ASSERT(strstr(error, "only through subagent") != NULL);
        sirio_model_store_destroy(store);
    }

    char *unchanged = NULL;
    size_t unchanged_length = 0;
    TEST_ASSERT(provider_read_auth_file(
        path, &unchanged, &unchanged_length) == 0);
    TEST_ASSERT(unchanged_length == sizeof(partial) - 1);
    TEST_ASSERT(unchanged && !memcmp(
        unchanged, partial, sizeof(partial) - 1));
    free(unchanged);
    unlink(path);
}

static void test_model_store_persists_single_interface_selection(void) {
    char path[] = "/tmp/sirio-models-XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0);
    if (fd < 0) return;
    static const char configured[] =
        "{\"interface\":{\"models\":[\"openai/gpt-5.6-luna\","
        "\"deepseek/deepseek-v4-pro\"],"
        "\"last_used\":{\"model\":null}},"
        "\"openai\":[{\"id\":\"gpt-5.6-luna\","
        "\"last_effort\":\"low\",\"active\":true}],"
        "\"deepseek\":[{\"id\":\"deepseek-v4-pro\","
        "\"last_effort\":\"high\",\"active\":true},{"
        "\"id\":\"deepseek-v4-flash\",\"last_effort\":\"high\","
        "\"active\":false}],"
        "\"opencode-go\":[{\"id\":\"glm-5.3\","
        "\"last_effort\":\"high\",\"active\":true}]}\n";
    TEST_ASSERT(write(fd, configured, sizeof(configured) - 1) ==
                (ssize_t)(sizeof(configured) - 1));
    TEST_ASSERT(close(fd) == 0);

    char error[256] = {0};
    sirio_model_store *store = sirio_model_store_load(
        path, error, sizeof(error));
    TEST_ASSERT(store != NULL);
    if (!store) return;
    TEST_ASSERT(sirio_model_store_count(
        store, SIRIO_PROVIDER_OPENAI) == 1);
    TEST_ASSERT(sirio_model_store_count(
        store, SIRIO_PROVIDER_DEEPSEEK) == 2);
    TEST_ASSERT(sirio_model_store_count(
        store, SIRIO_PROVIDER_OPENCODE_GO) == 1);
    TEST_ASSERT(sirio_model_store_count(
        store, SIRIO_PROVIDER_KIMI) == 0);
    const sirio_model_info *luna = sirio_model_store_resolve(
        store, SIRIO_PROVIDER_NONE, OPENAI_MODEL,
        error, sizeof(error));
    TEST_ASSERT(luna && !strcmp(luna->name, OPENAI_MODEL));
    TEST_ASSERT(sirio_model_store_resolve(
        store, SIRIO_PROVIDER_NONE, "luna", error, sizeof(error)) == NULL);
    TEST_ASSERT(sirio_model_store_resolve_interface(
        store, "deepseek/deepseek-v4-flash",
        error, sizeof(error)) == NULL);
    TEST_ASSERT(strstr(error, "inactive") != NULL);
    TEST_ASSERT(sirio_model_store_set_last_used(
        store, luna, SIRIO_REASONING_HIGH) == 0);
    const sirio_model_info *deepseek_pro = sirio_model_find_for_provider(
        SIRIO_PROVIDER_DEEPSEEK, DEEPSEEK_PRO_MODEL);
    TEST_ASSERT(sirio_model_store_set_last_used(
        store, deepseek_pro, SIRIO_REASONING_MAX) == 0);
    const sirio_model_info *glm = sirio_model_find_for_provider(
        SIRIO_PROVIDER_OPENCODE_GO, OPENCODE_GO_GLM_MODEL);
    TEST_ASSERT(sirio_model_store_set_last_used(
        store, glm, SIRIO_REASONING_HIGH) != 0);
    TEST_ASSERT(sirio_model_store_set_last_used(
        store, luna, SIRIO_REASONING_HIGH) == 0);
    TEST_ASSERT(sirio_model_store_save(store, path,
                                       error, sizeof(error)) == 0);
    sirio_model_store_destroy(store);

    char *text = NULL;
    size_t length = 0;
    TEST_ASSERT(provider_read_auth_file(path, &text, &length) == 0);
    TEST_ASSERT(text && strstr(text, "\"openai\": ["));
    TEST_ASSERT(text && !strstr(text, "\"kimi\":"));
    TEST_ASSERT(text && !strstr(text, "\"alias\""));
    TEST_ASSERT(text && strstr(text, "\"last_effort\": \"high\""));
    TEST_ASSERT(text && strstr(text, "\"active\": true"));
    TEST_ASSERT(text && strstr(text,
        "\"last_used\": {\"model\": \"openai/gpt-5.6-luna\"}"));
    TEST_ASSERT(text && !strstr(text, "\"provider\":"));
    TEST_ASSERT(text && !strstr(text, "context_tokens"));
    TEST_ASSERT(text && !strstr(text, "max_output_tokens"));
    free(text);

    store = sirio_model_store_load(path, error, sizeof(error));
    TEST_ASSERT(store != NULL);
    if (store) {
        const sirio_model_info *model = NULL;
        TEST_ASSERT(sirio_model_store_last_used(store, &model));
        TEST_ASSERT(model == sirio_model_find(OPENAI_MODEL));
        TEST_ASSERT(sirio_model_store_effort(store, model) ==
                    SIRIO_REASONING_HIGH);
        sirio_model_store_destroy(store);
    }
    unlink(path);
}

static void test_chat_provider_reasoning_requests(void) {
    sirio_generation_options options = {
        .temperature = -1,
        .top_p = -1,
        .reasoning = SIRIO_REASONING_NONE,
    };
    sirio_message message = {
        .role = SIRIO_ROLE_USER,
        .content = "hello",
    };
    bridge_buffer request = {0};
    TEST_ASSERT(build_request_for_model(
        &request, SIRIO_PROVIDER_OPENCODE_GO, OPENCODE_GO_MODEL,
        &options, &message, 1, NULL, 0) == 0);
    TEST_ASSERT(strstr(request.data,
                       "\"thinking\":{\"type\":\"disabled\"}") != NULL);
    TEST_ASSERT(strstr(request.data, "reasoning_effort") == NULL);
    buffer_free(&request);

    options.reasoning = SIRIO_REASONING_LOW;
    TEST_ASSERT(build_request_for_model(
        &request, SIRIO_PROVIDER_OPENCODE_GO, OPENCODE_GO_GLM_MODEL,
        &options, &message, 1, NULL, 0) == 0);
    TEST_ASSERT(strstr(request.data, "\"thinking\":{\"type\":\"enabled\"}") != NULL);
    TEST_ASSERT(strstr(request.data, "\"reasoning_effort\":\"low\"") != NULL);
    buffer_free(&request);

    options.reasoning = SIRIO_REASONING_HIGH;
    TEST_ASSERT(build_request_for_model(
        &request, SIRIO_PROVIDER_KIMI, KIMI_CODING_MODEL,
        &options, &message, 1, NULL, 0) == 0);
    TEST_ASSERT(strstr(request.data, "\"thinking\":{\"type\":\"enabled\"}") != NULL);
    TEST_ASSERT(strstr(request.data, "reasoning_effort") == NULL);
    buffer_free(&request);

    sirio_message replay[] = {
        {.role = SIRIO_ROLE_USER, .content = "use a tool"},
        {.role = SIRIO_ROLE_ASSISTANT, .provider = SIRIO_PROVIDER_KIMI,
         .content = "done", .reasoning = "because"},
    };
    TEST_ASSERT(build_request_for_model(
        &request, SIRIO_PROVIDER_KIMI, KIMI_MODEL,
        &options, replay, 2, NULL, 0) == 0);
    TEST_ASSERT(strstr(request.data, "\"reasoning_content\":\"because\"") != NULL);
    buffer_free(&request);
}

static void test_openai_responses_request_and_replay(void) {
    sirio_generation_options options = {
        .max_tokens = 4096,
        .temperature = -1,
        .top_p = -1,
        .reasoning = SIRIO_REASONING_HIGH,
    };
    sirio_tool_call call = {
        .id = "call_1", .name = "read",
        .arguments_json = "{\"path\":\"sirio.c\"}",
    };
    static char state[] =
        "[{\"type\":\"reasoning\",\"encrypted_content\":\"opaque\"},"
        "{\"type\":\"function_call\",\"call_id\":\"call_1\","
        "\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"sirio.c\\\"}\"}]";
    sirio_message messages[] = {
        {.role=SIRIO_ROLE_SYSTEM, .content="system"},
        {.role=SIRIO_ROLE_USER, .content="read it"},
        {.role=SIRIO_ROLE_ASSISTANT, .content="", .provider_state_json=state,
         .tool_calls=&call, .tool_call_count=1},
        {.role=SIRIO_ROLE_TOOL, .content="contents", .tool_call_id="call_1"},
    };
    sirio_tool tool = {
        .name="read", .description="Read a file",
        .input_schema_json="{\"type\":\"object\",\"properties\":{}}",
    };
    bridge_buffer request = {0};
    TEST_ASSERT(openai_build_request(&request, &options, messages, 4,
                                     &tool, 1) == 0);
    TEST_ASSERT(strstr(request.data, "\"model\":\"" OPENAI_MODEL "\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"instructions\"") == NULL);
    TEST_ASSERT(strstr(request.data, "\"type\":\"additional_tools\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"role\":\"developer\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"encrypted_content\":\"opaque\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"type\":\"function_call_output\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"output\":\"contents\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"strict\":false") != NULL);
    TEST_ASSERT(strstr(request.data, "\"effort\":\"high\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"context\":\"all_turns\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"verbosity\":\"low\"") != NULL);
    TEST_ASSERT(strstr(request.data, "\"max_output_tokens\"") == NULL);
    TEST_ASSERT(strstr(request.data,
                       "\"include\":[\"reasoning.encrypted_content\"]") != NULL);
    buffer_free(&request);

    sirio_message raw_message = {
        .role = SIRIO_ROLE_USER,
        .content = "hello",
    };
    TEST_ASSERT(openai_build_request(&request, &options, &raw_message, 1,
                                     NULL, 0) == 0);
    TEST_ASSERT(strstr(request.data, "\"parallel_tool_calls\":false") != NULL);
    TEST_ASSERT(strstr(request.data, "\"tool_choice\"") == NULL);
    buffer_free(&request);

    TEST_ASSERT(openai_build_request_for_transport(
        &request, OPENAI_MODEL, true, &options, messages, 4,
        &tool, 1) == 0);
    TEST_ASSERT(strstr(request.data, "\"max_output_tokens\":4096") != NULL);
    TEST_ASSERT(strstr(request.data, "\"effort\":\"high\"") != NULL);
    buffer_free(&request);
}

static void test_openai_responses_stream(void) {
    stream_capture capture = {0};
    openai_stream stream = {
        .callback = capture_stream_event,
        .private_data = &capture,
    };
    char header[] = "Content-Type: text/event-stream\r\n";
    TEST_ASSERT(openai_stream_header(header, 1, strlen(header), &stream) ==
                strlen(header));
    const char *body =
        "event: response.reasoning_summary_text.delta\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"why\"}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"reasoning\",\"encrypted_content\":\"opaque\"}}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"x\\\"}\"}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"model\":\"gpt-test\",\"usage\":{\"input_tokens\":10,\"output_tokens\":6,\"total_tokens\":16,\"input_tokens_details\":{\"cached_tokens\":4},\"output_tokens_details\":{\"reasoning_tokens\":2}}}}\n\n";
    size_t length = strlen(body);
    for (size_t offset = 0; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > 9) chunk = 9;
        TEST_ASSERT(openai_stream_write((char *)body + offset, 1, chunk,
                                        &stream) == chunk);
        offset += chunk;
    }
    TEST_ASSERT(stream.completed);
    TEST_ASSERT(!stream.failed);
    TEST_ASSERT(capture.reasoning.data && !strcmp(capture.reasoning.data, "why"));
    TEST_ASSERT(capture.text.data && !strcmp(capture.text.data, "hello"));
    TEST_ASSERT(capture.tool_events == 1);
    TEST_ASSERT(!strcmp(capture.tool_call_id, "call_1"));
    TEST_ASSERT(capture.provider_state_events == 1);
    TEST_ASSERT(capture.provider_state.data &&
                strstr(capture.provider_state.data,
                       "\"encrypted_content\":\"opaque\"") != NULL);
    TEST_ASSERT(capture.usage_events == 1);
    TEST_ASSERT(capture.prompt_tokens == 10);
    TEST_ASSERT(capture.cache_hit_tokens == 4);
    TEST_ASSERT(capture.reasoning_tokens == 2);
    TEST_ASSERT(capture.done_events == 1);
    TEST_ASSERT(!strcmp(capture.finish_reason, "tool_calls"));
    TEST_ASSERT(!strcmp(capture.request_id, "resp_1"));
    openai_stream_free(&stream);
    stream_capture_free(&capture);

    stream_capture buffered_capture = {0};
    openai_stream buffered = {
        .callback = capture_stream_event,
        .private_data = &buffered_capture,
    };
    TEST_ASSERT(openai_stream_write((char *)body, 1, strlen(body),
                                    &buffered) == strlen(body));
    TEST_ASSERT(!buffered.is_sse);
    TEST_ASSERT(openai_stream_parse_buffered_body(&buffered) == 0);
    TEST_ASSERT(buffered.completed);
    TEST_ASSERT(buffered_capture.text.data &&
                !strcmp(buffered_capture.text.data, "hello"));
    TEST_ASSERT(buffered_capture.done_events == 1);
    openai_stream_free(&buffered);
    stream_capture_free(&buffered_capture);
}

static int test_cancel_poll(void *private_data) {
    int *cancel = private_data;
    return *cancel;
}

static void test_cancellation_progress_contract(void) {
    sirio_bridge bridge = {0};
    atomic_init(&bridge.cancelled, false);
    int cancel = 0;
    sirio_bridge_set_cancel_poll(&bridge, test_cancel_poll, &cancel);
    TEST_ASSERT(bridge.cancel_poll == test_cancel_poll);
    TEST_ASSERT(bridge.cancel_poll_priv == &cancel);
    TEST_ASSERT(http_progress(&bridge, 0, 0, 0, 0) == 0);

    cancel = 1;
    TEST_ASSERT(http_progress(&bridge, 0, 0, 0, 0) == 1);
    cancel = 0;
    sirio_bridge_cancel(&bridge);
    TEST_ASSERT(http_progress(&bridge, 0, 0, 0, 0) == 1);

    sirio_bridge_set_cancel_poll(&bridge, NULL, NULL);
    TEST_ASSERT(bridge.cancel_poll == NULL);
    TEST_ASSERT(bridge.cancel_poll_priv == NULL);
}

int main(void) {
    test_non_thinking_sampling_request();
    test_thinking_request();
    test_structured_tool_request_remains_optional();
    test_normalized_tool_exchange_maps_to_deepseek_wire();
    test_normalized_results_are_correlated_by_id();
    test_flat_tool_argument_decoder();
    test_invalid_generation_options();
    test_model_catalog_capabilities();
    test_sse_text_reasoning_usage_and_metadata();
    test_sse_tool_call_fragments();
    test_sse_rejects_callback_and_malformed_terminal_state();
    test_openai_pkce_sha256();
    test_openai_jwt_claims();
    test_openai_auth_round_trip();
    test_auth_store_rejects_wrong_shape();
    test_auth_store_accepts_missing_providers();
    test_model_store_rejects_wrong_shape();
    test_model_store_uses_configured_catalog_and_interface();
    test_model_store_persists_single_interface_selection();
    test_chat_provider_reasoning_requests();
    test_openai_responses_request_and_replay();
    test_openai_responses_stream();
    test_cancellation_progress_contract();
    if (test_failures) {
        fprintf(stderr, "sirio provider contract tests: %d failure(s)\n", test_failures);
        return 1;
    }
    puts("sirio provider contract tests: ok");
    return 0;
}
