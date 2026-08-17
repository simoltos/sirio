#define SIRIO_TOOL_RUNNER_TEST
#include "../container/sirio_tool_runner.c"

static int failures;

#define CHECK(condition) do {                                           \
        if (!(condition)) {                                             \
            fprintf(stderr, "%s:%d: assertion failed: %s\n",          \
                    __FILE__, __LINE__, #condition);                     \
            failures++;                                                 \
        }                                                               \
    } while (0)

static const sirio_tool_argument *find_argument(
        const sirio_tool_request *request, const char *name) {
    for (size_t i = 0; i < request->argument_count; i++)
        if (!strcmp(request->arguments[i].name, name))
            return &request->arguments[i];
    return NULL;
}

static void test_protocol_round_trip_shapes(void) {
    const char *json =
        "{\"tool\":\"read\",\"args\":{\"path\":\"a b\",\"whole\":true}}";
    sirio_tool_request request;
    char error[160] = {0};
    CHECK(sirio_tool_protocol_decode_request(
              json, &request, error, sizeof(error)) == 0);
    CHECK(request.tool && !strcmp(request.tool, "read"));
    CHECK(request.argument_count == 2);
    const sirio_tool_argument *path = find_argument(&request, "path");
    const sirio_tool_argument *whole = find_argument(&request, "whole");
    CHECK(path && path->is_string && !strcmp(path->value, "a b"));
    CHECK(whole && !whole->is_string && !strcmp(whole->value, "true"));
    sirio_tool_request_free(&request);

    error[0] = '\0';
    CHECK(sirio_tool_protocol_decode_request(
              "{\"tool\":\"read\",\"args\":{},\"extra\":1}",
              &request, error, sizeof(error)) != 0);
    CHECK(error[0] != '\0');

    char *response = sirio_tool_protocol_encode_response("line\n", false);
    CHECK(response && !strcmp(response, "{\"result\":\"line\\n\"}"));
    free(response);
    response = sirio_tool_protocol_encode_response("bad", true);
    CHECK(response && !strcmp(response, "{\"error\":\"bad\"}"));
    free(response);
}

static void test_google_block_classifier(void) {
    CHECK(web_google_blocked(
        "https://www.google.com/sorry/index?continue=x", ""));
    CHECK(web_google_blocked(
        "https://www.google.com/search?q=x",
        "Our systems have detected unusual traffic from your computer network."));
    CHECK(web_google_blocked(
        "https://www.google.com/search?q=x",
        "Your computer or network may be sending automated queries."));
    CHECK(!web_google_blocked(
        "https://example.com/consent", "I agree"));
    CHECK(!web_google_blocked(
        "https://www.google.com/search?q=x", "ordinary search results"));
}

static void test_http_content_length_boundary(void) {
    static char response_text[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Content-Length: 5\r\n\r\nhello";
    web_buf response = {
        .ptr = response_text,
        .len = sizeof(response_text) - 1,
        .cap = sizeof(response_text),
    };
    size_t expected = 0;
    CHECK(web_http_expected_size(&response, &expected));
    CHECK(expected == response.len);

    response.ptr = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello";
    response.len = strlen(response.ptr);
    CHECK(!web_http_expected_size(&response, &expected));
}

int main(void) {
    test_protocol_round_trip_shapes();
    test_google_block_classifier();
    test_http_content_length_boundary();
    if (failures) {
        fprintf(stderr, "sirio tool runner tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("sirio tool runner tests: ok");
    return 0;
}
