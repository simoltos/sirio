#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIRIO_NO_MAIN
#include "../sirio_provider.c"

static int terminal_failures;

#define TERMINAL_CHECK(expr)                                                   \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "%s:%d: assertion failed: %s\n",                \
                    __FILE__, __LINE__, #expr);                                \
            terminal_failures++;                                               \
        }                                                                      \
    } while (0)

typedef struct {
    int text_events;
    int reasoning_events;
    int tool_events;
    int usage_events;
    int done_events;
    int error_events;
    int provider_state_events;
    char order[32];
    size_t order_length;
} terminal_capture;

static void terminal_record(terminal_capture *capture, char marker)
{
    if (capture->order_length + 1 >= sizeof(capture->order)) {
        terminal_failures++;
        return;
    }
    capture->order[capture->order_length++] = marker;
    capture->order[capture->order_length] = '\0';
}

static int terminal_capture_event(const sirio_bridge_event *event,
                                  void *private_data)
{
    terminal_capture *capture = private_data;

    switch (event->type) {
    case SIRIO_BRIDGE_EVENT_TEXT:
        capture->text_events++;
        terminal_record(capture, 'X');
        break;
    case SIRIO_BRIDGE_EVENT_REASONING:
        capture->reasoning_events++;
        terminal_record(capture, 'R');
        break;
    case SIRIO_BRIDGE_EVENT_TOOL_CALL:
        capture->tool_events++;
        terminal_record(capture, 'T');
        break;
    case SIRIO_BRIDGE_EVENT_PROVIDER_STATE:
        capture->provider_state_events++;
        break;
    case SIRIO_BRIDGE_EVENT_USAGE:
        capture->usage_events++;
        break;
    case SIRIO_BRIDGE_EVENT_DONE:
        capture->done_events++;
        break;
    case SIRIO_BRIDGE_EVENT_ERROR:
        capture->error_events++;
        break;
    }
    return 0;
}

static void openai_terminal_init(openai_stream *stream,
                                 terminal_capture *capture)
{
    memset(stream, 0, sizeof(*stream));
    memset(capture, 0, sizeof(*capture));
    stream->callback = terminal_capture_event;
    stream->private_data = capture;
    stream->is_sse = 1;
    stream->content_type_seen = 1;
}

static int openai_terminal_feed_chunks(openai_stream *stream,
                                       const char *body, size_t chunk_size)
{
    size_t offset = 0;
    size_t length = strlen(body);

    while (offset < length) {
        size_t count = length - offset;
        if (count > chunk_size)
            count = chunk_size;
        if (openai_stream_feed(stream, body + offset, count) != 0)
            return -1;
        offset += count;
    }
    return 0;
}

static void openai_terminal_expect_error(const char *body, size_t chunk_size,
                                         const char *needle)
{
    openai_stream stream;
    terminal_capture capture;

    openai_terminal_init(&stream, &capture);
    TERMINAL_CHECK(openai_terminal_feed_chunks(&stream, body,
                                                chunk_size) != 0);
    TERMINAL_CHECK(strstr(stream.error, needle) != NULL);
    TERMINAL_CHECK(capture.done_events == 0);
    openai_stream_free(&stream);
}

static void terminal_response_init(stream_response *response,
                                   terminal_capture *capture)
{
    char header[] = "Content-Type: text/event-stream; charset=utf-8\r\n";
    size_t length = strlen(header);

    memset(response, 0, sizeof(*response));
    memset(capture, 0, sizeof(*capture));
    response->callback = terminal_capture_event;
    response->private_data = capture;
    TERMINAL_CHECK(stream_http_header(header, 1, length, response) == length);
}

static int terminal_feed_chunks(stream_response *response, const char *body,
                                size_t chunk_size)
{
    size_t offset = 0;
    size_t length = strlen(body);

    while (offset < length) {
        size_t count = length - offset;
        if (count > chunk_size)
            count = chunk_size;
        if (stream_feed_sse(response, body + offset, count) != 0)
            return -1;
        offset += count;
    }
    return 0;
}

static void terminal_expect_error(const char *body, size_t chunk_size,
                                  const char *needle)
{
    stream_response response;
    terminal_capture capture;
    int rc;

    terminal_response_init(&response, &capture);
    rc = terminal_feed_chunks(&response, body, chunk_size);
    if (rc == 0)
        rc = stream_finish(&response);
    TERMINAL_CHECK(rc != 0);
    if (strstr(response.error, needle) == NULL)
        fprintf(stderr, "expected error containing '%s', got '%s'\n",
                needle, response.error);
    TERMINAL_CHECK(strstr(response.error, needle) != NULL);
    TERMINAL_CHECK(capture.done_events == 0);
    stream_response_free(&response);
}

#define TERMINAL_USAGE                                                        \
    "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[],"       \
    "\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":1,"          \
    "\"total_tokens\":3}}\n\n"

#define TERMINAL_DONE "data: [DONE]\n\n"

static void test_empty_and_incomplete_terminal_states(void)
{
    static const char no_finish[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"content\":\"x\"},"                    \
        "\"finish_reason\":null}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    static const char empty_finish[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    static const char no_done[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        TERMINAL_USAGE;

    terminal_expect_error("", 1, "no chunks");
    terminal_expect_error(no_finish, 3, "no finish reason");
    terminal_expect_error(empty_finish, 5, "empty finish reason");
    terminal_expect_error(no_done, 7, "without [DONE]");
}

static void test_duplicate_and_post_done_events(void)
{
    static const char after_done[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[],"         \
        "\"usage\":null}\n\n";
    static const char duplicate_usage[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_USAGE TERMINAL_DONE;

    terminal_expect_error(after_done, 2, "data after");
    terminal_expect_error(duplicate_usage, 9, "duplicate");
}

static void test_nullable_usage_success(void)
{
    static const char body[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"content\":\"x\"},"                    \
        "\"finish_reason\":\"stop\"}],\"usage\":null}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    stream_response response;
    terminal_capture capture;

    terminal_response_init(&response, &capture);
    TERMINAL_CHECK(terminal_feed_chunks(&response, body, 4) == 0);
    TERMINAL_CHECK(stream_finish(&response) == 0);
    TERMINAL_CHECK(capture.text_events == 1);
    TERMINAL_CHECK(capture.usage_events == 1);
    TERMINAL_CHECK(capture.done_events == 1);
    stream_response_free(&response);
}

static void test_nullable_optional_chat_fields_success(void)
{
    static const char text_body[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"content\":\"x\","                    \
        "\"reasoning_content\":null,\"tool_calls\":null},"                 \
        "\"finish_reason\":\"stop\"}],\"usage\":null}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,"          \
        "\"completion_tokens\":1,\"total_tokens\":3,"                    \
        "\"completion_tokens_details\":null}}\n\n"
        TERMINAL_DONE;
    static const char tool_body[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"content\":null,\"tool_calls\":[{"    \
        "\"index\":0,\"id\":\"call\",\"type\":\"function\","          \
        "\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]},"     \
        "\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"          \
        "\"id\":null,\"type\":null,\"function\":{\"name\":null,"       \
        "\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"tool_calls\":null},"                   \
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    stream_response response;
    terminal_capture capture;

    terminal_response_init(&response, &capture);
    TERMINAL_CHECK(terminal_feed_chunks(&response, text_body, 3) == 0);
    TERMINAL_CHECK(stream_finish(&response) == 0);
    TERMINAL_CHECK(capture.text_events == 1);
    TERMINAL_CHECK(capture.usage_events == 1);
    TERMINAL_CHECK(capture.done_events == 1);
    stream_response_free(&response);

    terminal_response_init(&response, &capture);
    TERMINAL_CHECK(terminal_feed_chunks(&response, tool_body, 5) == 0);
    TERMINAL_CHECK(stream_finish(&response) == 0);
    TERMINAL_CHECK(capture.tool_events == 1);
    TERMINAL_CHECK(capture.usage_events == 1);
    TERMINAL_CHECK(capture.done_events == 1);
    stream_response_free(&response);
}

static void test_identity_and_finish_changes(void)
{
    static const char request_change[] =
        "data: {\"id\":\"req-a\",\"model\":\"m\",\"choices\":[{"      \
        "\"index\":0,\"delta\":{\"content\":\"a\"},"                    \
        "\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"req-b\",\"model\":\"m\",\"choices\":[{"      \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    static const char model_change[] =
        "data: {\"id\":\"req\",\"model\":\"m-a\",\"choices\":[{"      \
        "\"index\":0,\"delta\":{\"content\":\"a\"},"                    \
        "\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"req\",\"model\":\"m-b\",\"choices\":[{"      \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    static const char finish_change[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";

    terminal_expect_error(request_change, 5, "changed during stream");
    terminal_expect_error(model_change, 6, "changed during stream");
    terminal_expect_error(finish_change, 7, "finish reason changed");
}

static void test_incomplete_and_empty_tool_calls(void)
{
    static const char incomplete[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"          \
        "\"id\":\"call\",\"type\":\"function\",\"function\":{"         \
        "\"name\":\"read\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    static const char empty_delta[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"tool_calls\":[{}]},"                    \
        "\"finish_reason\":null}]}\n\n";

    terminal_expect_error(incomplete, 8, "incomplete");
    terminal_expect_error(empty_delta, 3, "empty tool");
}

static void test_invalid_utf8_fields(void)
{
    static const char bad_content[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"content\":\"" "\xC3\x28" "\"},"     \
        "\"finish_reason\":null}]}\n\n";
    static const char truncated_content[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"content\":\"" "\xC3" "\"},"         \
        "\"finish_reason\":null}]}\n\n";
    static const char bad_reasoning[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"reasoning_content\":\""               \
        "\xC3\x28" "\"},\"finish_reason\":null}]}\n\n";
    static const char bad_finish[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{},\"finish_reason\":\"" "\xC3\x28"     \
        "\"}]}\n\n";

    terminal_expect_error(bad_content, 2, "invalid UTF-8");
    terminal_expect_error(truncated_content, 2, "invalid UTF-8");
    terminal_expect_error(bad_reasoning, 2, "invalid UTF-8");
    terminal_expect_error(bad_finish, 2, "invalid UTF-8");
}

static void test_mixed_text_and_tool_order(void)
{
    static const char body[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"content\":\"before\"},"               \
        "\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"        \
        "\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"          \
        "\"id\":\"call\",\"type\":\"function\",\"function\":{"         \
        "\"name\":\"read\",\"arguments\":\"{}\"}}]},"                  \
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    stream_response response;
    terminal_capture capture;

    terminal_response_init(&response, &capture);
    TERMINAL_CHECK(terminal_feed_chunks(&response, body, 3) == 0);
    TERMINAL_CHECK(stream_finish(&response) == 0);
    TERMINAL_CHECK(strcmp(capture.order, "XT") == 0);
    TERMINAL_CHECK(capture.tool_events == 1);
    TERMINAL_CHECK(capture.usage_events == 1);
    TERMINAL_CHECK(capture.done_events == 1);
    stream_response_free(&response);
}

static void test_tool_finish_parity_and_unique_ids(void)
{
    static const char tool_finish_without_calls[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"
        "\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    static const char calls_with_stop_finish[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"
        "\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call\",\"type\":\"function\",\"function\":{"
        "\"name\":\"read\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;
    static const char duplicate_call_ids[] =
        "data: {\"id\":\"req\",\"model\":\"m\",\"choices\":[{"
        "\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"same\",\"type\":\"function\",\"function\":{"
        "\"name\":\"read\",\"arguments\":\"{}\"}},{\"index\":1,"
        "\"id\":\"same\",\"type\":\"function\",\"function\":{"
        "\"name\":\"list\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        TERMINAL_USAGE TERMINAL_DONE;

    terminal_expect_error(tool_finish_without_calls, 5,
                          "delivered no calls");
    terminal_expect_error(calls_with_stop_finish, 7,
                          "non-tool finish reason");
    terminal_expect_error(duplicate_call_ids, 9,
                          "duplicate Chat Completions tool-call id");
}

#define TERMINAL_STREAM_LIMIT ((size_t)16 * 1024 * 1024)

static void test_bounded_stream_limits(void)
{
    stream_response response;
    terminal_capture capture;
    char byte = 'x';
    char *fragment;
    char *accumulated = NULL;
    static const char data_line[] = "data: x\n";

    terminal_response_init(&response, &capture);
    response.pending.length = TERMINAL_STREAM_LIMIT;
    TERMINAL_CHECK(stream_feed_sse(&response, &byte, 1) != 0);
    TERMINAL_CHECK(strstr(response.error, "SSE buffer exceeded") != NULL);
    stream_response_free(&response);

    terminal_response_init(&response, &capture);
    response.event_data.length = TERMINAL_STREAM_LIMIT;
    TERMINAL_CHECK(stream_feed_sse(&response, data_line,
                                   sizeof(data_line) - 1) != 0);
    TERMINAL_CHECK(strstr(response.error, "SSE event exceeded") != NULL);
    stream_response_free(&response);

    terminal_response_init(&response, &capture);
    response.received = TERMINAL_STREAM_LIMIT;
    TERMINAL_CHECK(stream_http_write(&byte, 1, 1, &response) == 0);
    TERMINAL_CHECK(strstr(response.error, "response exceeded") != NULL);
    stream_response_free(&response);

    fragment = malloc(TERMINAL_STREAM_LIMIT + 2);
    TERMINAL_CHECK(fragment != NULL);
    if (fragment != NULL) {
        memset(fragment, 'x', TERMINAL_STREAM_LIMIT + 1);
        fragment[TERMINAL_STREAM_LIMIT + 1] = '\0';
        terminal_response_init(&response, &capture);
        TERMINAL_CHECK(stream_append_fragment(&response, &accumulated,
                                              fragment,
                                              "tool-call fragment") != 0);
        TERMINAL_CHECK(strstr(response.error, "exceeded limits") != NULL);
        TERMINAL_CHECK(accumulated == NULL);
        stream_response_free(&response);
        free(fragment);
    }
    free(accumulated);
}

static void test_openai_terminal_success_and_order(void)
{
    static const char body[] =
        "data: {\"type\":\"response.reasoning_summary_text.delta\","
        "\"delta\":\"why\"}\n\n"
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"ok\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{"
        "\"type\":\"function_call\",\"call_id\":\"call-1\","
        "\"name\":\"read\",\"arguments\":\"{}\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"id\":\"resp-1\",\"model\":\"gpt-test\",\"usage\":{"
        "\"input_tokens\":2,\"output_tokens\":1,\"total_tokens\":3}}}\n\n";
    openai_stream stream;
    terminal_capture capture;

    openai_terminal_init(&stream, &capture);
    TERMINAL_CHECK(openai_terminal_feed_chunks(&stream, body, 3) == 0);
    TERMINAL_CHECK(stream.completed);
    TERMINAL_CHECK(strcmp(capture.order, "RXT") == 0);
    TERMINAL_CHECK(capture.reasoning_events == 1);
    TERMINAL_CHECK(capture.text_events == 1);
    TERMINAL_CHECK(capture.tool_events == 1);
    TERMINAL_CHECK(capture.usage_events == 1);
    TERMINAL_CHECK(capture.provider_state_events == 1);
    TERMINAL_CHECK(capture.done_events == 1);
    openai_stream_free(&stream);
}

static void test_openai_terminal_item_summary_fallback(void)
{
    static const char body[] =
        "data: {\"type\":\"response.output_item.done\",\"item\":{"
        "\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\","
        "\"text\":\"fallback summary\"}]}}\n\n"
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"answer\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"id\":\"resp-1\",\"model\":\"gpt-test\"}}\n\n";
    openai_stream stream;
    terminal_capture capture;

    openai_terminal_init(&stream, &capture);
    TERMINAL_CHECK(openai_terminal_feed_chunks(&stream, body, 5) == 0);
    TERMINAL_CHECK(stream.completed);
    TERMINAL_CHECK(!strcmp(capture.order, "RX"));
    TERMINAL_CHECK(capture.reasoning_events == 1);
    TERMINAL_CHECK(capture.text_events == 1);
    openai_stream_free(&stream);
}

static void test_openai_terminal_failures(void)
{
    static const char missing_type[] = "data: {\"delta\":\"x\"}\n\n";
    static const char malformed_delta[] =
        "data: {\"type\":\"response.output_text.delta\"}\n\n";
    static const char incomplete_call[] =
        "data: {\"type\":\"response.output_item.done\",\"item\":{"
        "\"type\":\"function_call\",\"call_id\":\"call-1\","
        "\"name\":\"read\"}}\n\n";
    static const char failed[] =
        "data: {\"type\":\"response.failed\","
        "\"message\":\"provider failed\"}\n\n";
    static const char after_completed[] =
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"id\":\"resp-1\",\"model\":\"gpt-test\"}}\n\n"
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"late\"}\n\n";
    openai_stream stream;
    terminal_capture capture;

    openai_terminal_expect_error(missing_type, 2, "has no type");
    openai_terminal_expect_error(malformed_delta, 5, "delta is malformed");
    openai_terminal_expect_error(incomplete_call, 7, "incomplete OpenAI");
    openai_terminal_expect_error(failed, 4, "provider failed");
    openai_terminal_init(&stream, &capture);
    TERMINAL_CHECK(openai_terminal_feed_chunks(&stream, after_completed, 6) != 0);
    TERMINAL_CHECK(strstr(stream.error, "data after") != NULL);
    TERMINAL_CHECK(capture.done_events == 1);
    openai_stream_free(&stream);

    openai_terminal_init(&stream, &capture);
    TERMINAL_CHECK(openai_terminal_feed_chunks(
                       &stream,
                       "data: {\"type\":\"response.output_text.delta\","
                       "\"delta\":\"unfinished\"}\n\n",
                       3) == 0);
    TERMINAL_CHECK(!stream.completed);
    TERMINAL_CHECK(capture.done_events == 0);
    openai_stream_free(&stream);
}

static void test_openai_bounded_stream_limits(void)
{
    openai_stream stream;
    terminal_capture capture;
    char byte = 'x';
    static const char data_line[] = "data: x\n";

    openai_terminal_init(&stream, &capture);
    stream.pending.length = OPENAI_RESPONSE_LIMIT;
    TERMINAL_CHECK(openai_stream_feed(&stream, &byte, 1) != 0);
    TERMINAL_CHECK(strstr(stream.error, "SSE buffer exceeded") != NULL);
    openai_stream_free(&stream);

    openai_terminal_init(&stream, &capture);
    stream.event_data.length = OPENAI_RESPONSE_LIMIT;
    TERMINAL_CHECK(openai_stream_feed(&stream, data_line,
                                      sizeof(data_line) - 1) != 0);
    TERMINAL_CHECK(strstr(stream.error, "SSE event exceeded") != NULL);
    openai_stream_free(&stream);

    openai_terminal_init(&stream, &capture);
    stream.received = OPENAI_RESPONSE_LIMIT;
    TERMINAL_CHECK(openai_stream_write(&byte, 1, 1, &stream) == 0);
    TERMINAL_CHECK(strstr(stream.error, "response exceeded") != NULL);
    openai_stream_free(&stream);
}

int main(void)
{
    test_empty_and_incomplete_terminal_states();
    test_duplicate_and_post_done_events();
    test_nullable_usage_success();
    test_nullable_optional_chat_fields_success();
    test_identity_and_finish_changes();
    test_incomplete_and_empty_tool_calls();
    test_invalid_utf8_fields();
    test_mixed_text_and_tool_order();
    test_tool_finish_parity_and_unique_ids();
    test_bounded_stream_limits();
    test_openai_terminal_success_and_order();
    test_openai_terminal_item_summary_fallback();
    test_openai_terminal_failures();
    test_openai_bounded_stream_limits();

    if (terminal_failures != 0) {
        fprintf(stderr, "sirio provider terminal tests: %d failure(s)\n",
                terminal_failures);
        return 1;
    }
    puts("sirio provider terminal tests: ok");
    return 0;
}
