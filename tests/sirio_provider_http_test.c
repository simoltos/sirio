#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SIRIO_NO_MAIN

static const char *http_endpoint_override;
static const char http_blocked_endpoint[] = "http://127.0.0.1:1/unavailable";
static long http_timeout_override = -1;
static int http_fail_global_init;
static int http_fail_easy_init;
static int http_fail_slist_after = -1;
static int http_slist_calls;
static long http_fail_allocation_after = -1;
static long http_allocation_calls;

static int http_allocation_should_fail(void)
{
    long call = http_allocation_calls++;
    return http_fail_allocation_after >= 0 &&
           call >= http_fail_allocation_after;
}

static void *http_malloc(size_t size)
{
    if (http_allocation_should_fail())
        return NULL;
    return (malloc)(size);
}

static void *http_calloc(size_t count, size_t size)
{
    if (http_allocation_should_fail())
        return NULL;
    return (calloc)(count, size);
}

static void *http_realloc(void *pointer, size_t size)
{
    if (http_allocation_should_fail())
        return NULL;
    return (realloc)(pointer, size);
}

static CURLcode http_curl_global_init(long flags)
{
    if (http_fail_global_init)
        return CURLE_FAILED_INIT;
    return (curl_global_init)(flags);
}

static CURL *http_curl_easy_init(void)
{
    if (http_fail_easy_init)
        return NULL;
    return (curl_easy_init)();
}

static struct curl_slist *http_curl_slist_append(struct curl_slist *list,
                                                 const char *value)
{
    if (http_fail_slist_after >= 0 &&
        http_slist_calls++ >= http_fail_slist_after)
        return NULL;
    return (curl_slist_append)(list, value);
}

#undef curl_easy_setopt
#define curl_global_init(flags) http_curl_global_init(flags)
#define curl_easy_init() http_curl_easy_init()
#define curl_slist_append(list, value) http_curl_slist_append((list), (value))
#define malloc(size) http_malloc(size)
#define calloc(count, size) http_calloc((count), (size))
#define realloc(pointer, size) http_realloc((pointer), (size))
#define curl_easy_setopt(handle, option, value)                                \
    (((option) == CURLOPT_URL)                                                  \
         ? (curl_easy_setopt)((handle), (option),                              \
                              http_endpoint_override != NULL                   \
                                  ? http_endpoint_override                     \
                                  : http_blocked_endpoint)                     \
         : ((option) == CURLOPT_TIMEOUT && http_timeout_override >= 0)         \
               ? (curl_easy_setopt)((handle), (option), http_timeout_override)\
               : (curl_easy_setopt)((handle), (option), (value)))

#include "../sirio_provider.c"

#undef realloc
#undef calloc
#undef malloc
#undef curl_easy_setopt
#undef curl_slist_append
#undef curl_easy_init
#undef curl_global_init

static int http_failures;

#define HTTP_CHECK(expr)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "%s:%d: assertion failed: %s\n",                \
                    __FILE__, __LINE__, #expr);                                \
            http_failures++;                                                   \
        }                                                                      \
    } while (0)

#define HTTP_SECRET "sirio-http-test-secret"
#define HTTP_REQUEST_CAPACITY (64 * 1024)

static const char valid_sse[] =
    "data: {\"id\":\"req-http\",\"model\":\"deepseek-test\","          \
    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"     \
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: {\"id\":\"req-http\",\"model\":\"deepseek-test\","          \
    "\"choices\":[],\"usage\":{\"prompt_tokens\":2,"                    \
    "\"completion_tokens\":1,\"total_tokens\":3}}\n\n"
    "data: [DONE]\n\n";

static const char valid_opencode_sse[] =
    "data: {\"id\":\"req-http\",\"model\":\"deepseek-test\","
    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
    "\"finish_reason\":null}],\"usage\":{\"prompt_tokens\":2,"
    "\"completion_tokens\":1,\"total_tokens\":3}}\n\n"
    "data: {\"id\":\"req-http\",\"model\":\"deepseek-test\","
    "\"choices\":[{\"index\":0,\"delta\":{},"
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: {\"id\":\"req-http\",\"model\":\"deepseek-test\","
    "\"choices\":[],\"usage\":{\"prompt_tokens\":2,"
    "\"completion_tokens\":2,\"total_tokens\":4}}\n\n"
    "data: [DONE]\n\n"
    "data: {\"choices\":[],\"cost\":\"0\"}\n\n";

static const char valid_openai_sse[] =
    "data: {\"type\":\"response.output_text.delta\","
    "\"delta\":\"ok\"}\n\n"
    "data: {\"type\":\"response.completed\",\"response\":{"
    "\"id\":\"resp-http\",\"model\":\"gpt-test\",\"usage\":{"
    "\"input_tokens\":2,\"output_tokens\":1,\"total_tokens\":3}}}\n\n";

typedef struct {
    int listener;
    pthread_t thread;
    char *response;
    size_t response_length;
    unsigned delay_ms;
    unsigned hold_ms;
    char request[HTTP_REQUEST_CAPACITY];
    size_t request_length;
    int accepted;
    int thread_started;
} http_fixture;

typedef struct {
    int text_events;
    int usage_events;
    int done_events;
    int error_events;
    int reject_text;
    char error[384];
} http_capture;

static void http_sleep_ms(unsigned milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static const char *http_find_bytes(const char *haystack, size_t haystack_length,
                                   const char *needle, size_t needle_length)
{
    size_t offset;

    if (needle_length == 0)
        return haystack;
    if (needle_length > haystack_length)
        return NULL;
    for (offset = 0; offset <= haystack_length - needle_length; offset++) {
        if (memcmp(haystack + offset, needle, needle_length) == 0)
            return haystack + offset;
    }
    return NULL;
}

static int http_ascii_equal_ci(char left, char right)
{
    if (left >= 'A' && left <= 'Z')
        left = (char)(left - 'A' + 'a');
    if (right >= 'A' && right <= 'Z')
        right = (char)(right - 'A' + 'a');
    return left == right;
}

static const char *http_find_ascii_ci(const char *haystack, size_t length,
                                      const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t offset;

    if (needle_length == 0)
        return haystack;
    if (needle_length > length)
        return NULL;
    for (offset = 0; offset <= length - needle_length; offset++) {
        size_t index;

        for (index = 0; index < needle_length; index++) {
            if (!http_ascii_equal_ci(haystack[offset + index], needle[index]))
                break;
        }
        if (index == needle_length)
            return haystack + offset;
    }
    return NULL;
}

static size_t http_request_target(const char *request, size_t length)
{
    const char *header_end;
    const char *content_length;
    size_t header_length;
    unsigned long long body_length = 0;

    header_end = http_find_bytes(request, length, "\r\n\r\n", 4);
    if (header_end == NULL)
        return 0;
    header_length = (size_t)(header_end - request) + 4;
    content_length = http_find_ascii_ci(request,
                                        (size_t)(header_end - request),
                                        "Content-Length:");
    if (content_length != NULL) {
        content_length += strlen("Content-Length:");
        body_length = strtoull(content_length, NULL, 10);
    }
    if (body_length > HTTP_REQUEST_CAPACITY - header_length - 1)
        return HTTP_REQUEST_CAPACITY;
    return header_length + (size_t)body_length;
}

static int http_send_all(int socket_fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
#ifdef MSG_NOSIGNAL
        ssize_t count = send(socket_fd, data + sent, length - sent,
                             MSG_NOSIGNAL);
#else
        ssize_t count = send(socket_fd, data + sent, length - sent, 0);
#endif
        if (count <= 0)
            return -1;
        sent += (size_t)count;
    }
    return 0;
}

static void *http_fixture_main(void *private_data)
{
    http_fixture *fixture = private_data;
    struct timeval timeout = {5, 0};
    int client;

    client = accept(fixture->listener, NULL, NULL);
    if (client < 0)
        return NULL;
    fixture->accepted = 1;
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (fixture->request_length + 1 < sizeof(fixture->request)) {
        size_t target;
        ssize_t count = recv(client,
                             fixture->request + fixture->request_length,
                             sizeof(fixture->request) -
                                 fixture->request_length - 1,
                             0);
        if (count <= 0)
            break;
        fixture->request_length += (size_t)count;
        fixture->request[fixture->request_length] = '\0';
        target = http_request_target(fixture->request,
                                     fixture->request_length);
        if (target != 0 && fixture->request_length >= target)
            break;
    }

    if (fixture->delay_ms != 0)
        http_sleep_ms(fixture->delay_ms);
    if (fixture->response_length != 0)
        (void)http_send_all(client, fixture->response,
                            fixture->response_length);
    if (fixture->hold_ms != 0)
        http_sleep_ms(fixture->hold_ms);
    (void)shutdown(client, SHUT_RDWR);
    close(client);
    return NULL;
}

static int http_fixture_start_raw(http_fixture *fixture,
                                  const char *response,
                                  size_t response_length,
                                  unsigned delay_ms,
                                  unsigned hold_ms)
{
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    char endpoint[128];
    int enabled = 1;

    memset(fixture, 0, sizeof(*fixture));
    fixture->listener = -1;
    fixture->response = malloc(response_length + 1);
    if (fixture->response == NULL)
        return -1;
    memcpy(fixture->response, response, response_length);
    fixture->response[response_length] = '\0';
    fixture->response_length = response_length;
    fixture->delay_ms = delay_ms;
    fixture->hold_ms = hold_ms;

    fixture->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (fixture->listener < 0)
        goto fail;
    (void)setsockopt(fixture->listener, SOL_SOCKET, SO_REUSEADDR,
                     &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fixture->listener, (struct sockaddr *)&address,
             sizeof(address)) != 0 ||
        listen(fixture->listener, 1) != 0 ||
        getsockname(fixture->listener, (struct sockaddr *)&address,
                    &address_length) != 0)
        goto fail;

    if (snprintf(endpoint, sizeof(endpoint),
                 "http://127.0.0.1:%u/chat/completions",
                 (unsigned)ntohs(address.sin_port)) >= (int)sizeof(endpoint))
        goto fail;
    http_endpoint_override = strdup(endpoint);
    if (http_endpoint_override == NULL)
        goto fail;
    if (pthread_create(&fixture->thread, NULL, http_fixture_main, fixture) != 0)
        goto fail;
    fixture->thread_started = 1;
    return 0;

fail:
    free((char *)http_endpoint_override);
    http_endpoint_override = NULL;
    if (fixture->listener >= 0) {
        close(fixture->listener);
        fixture->listener = -1;
    }
    free(fixture->response);
    fixture->response = NULL;
    return -1;
}

static int http_fixture_start(http_fixture *fixture, int status,
                              const char *content_type, const char *body,
                              size_t declared_extra, unsigned delay_ms,
                              unsigned hold_ms)
{
    const char *reason = status == 200 ? "OK" : "Test Error";
    const char *connection = hold_ms == 0 ? "close" : "keep-alive";
    size_t body_length = body != NULL ? strlen(body) : 0;
    size_t capacity = body_length + 512;
    char *response;
    int header_length;
    int result;

    memset(fixture, 0, sizeof(*fixture));
    fixture->listener = -1;
    response = malloc(capacity);
    if (response == NULL)
        return -1;
    header_length = snprintf(
        response, capacity,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: %s\r\n\r\n",
        status, reason, content_type != NULL ? content_type : "text/plain",
        body_length + declared_extra, connection);
    if (header_length < 0 || (size_t)header_length + body_length >= capacity) {
        free(response);
        return -1;
    }
    if (body_length != 0)
        memcpy(response + header_length, body, body_length);
    result = http_fixture_start_raw(fixture, response,
                                    (size_t)header_length + body_length,
                                    delay_ms, hold_ms);
    free(response);
    return result;
}

static void http_fixture_join(http_fixture *fixture)
{
    if (fixture->thread_started) {
        (void)pthread_join(fixture->thread, NULL);
        fixture->thread_started = 0;
    }
    if (fixture->listener >= 0) {
        close(fixture->listener);
        fixture->listener = -1;
    }
    free(fixture->response);
    fixture->response = NULL;
    free((char *)http_endpoint_override);
    http_endpoint_override = NULL;
}

static void http_fixture_abort(http_fixture *fixture)
{
    if (fixture->listener >= 0) {
        (void)shutdown(fixture->listener, SHUT_RDWR);
        close(fixture->listener);
        fixture->listener = -1;
    }
    http_fixture_join(fixture);
}

#define HTTP_FIXTURE_REQUIRE(expression, fixture, bridge)                      \
    do {                                                                       \
        if ((expression) != 0) {                                               \
            HTTP_CHECK(0);                                                     \
            http_fixture_join(&(fixture));                                     \
            http_timeout_override = -1;                                        \
            sirio_bridge_destroy((bridge));                                    \
            return;                                                            \
        }                                                                      \
    } while (0)

static int http_capture_event(const sirio_bridge_event *event,
                              void *private_data)
{
    http_capture *capture = private_data;

    switch (event->type) {
    case SIRIO_BRIDGE_EVENT_TEXT:
        capture->text_events++;
        if (capture->reject_text)
            return -1;
        break;
    case SIRIO_BRIDGE_EVENT_USAGE:
        capture->usage_events++;
        break;
    case SIRIO_BRIDGE_EVENT_DONE:
        capture->done_events++;
        break;
    case SIRIO_BRIDGE_EVENT_ERROR:
        capture->error_events++;
        snprintf(capture->error, sizeof(capture->error), "%s",
                 event->error != NULL ? event->error : "");
        break;
    case SIRIO_BRIDGE_EVENT_REASONING:
    case SIRIO_BRIDGE_EVENT_TOOL_CALL:
    case SIRIO_BRIDGE_EVENT_PROVIDER_STATE:
        break;
    }
    return 0;
}

static sirio_bridge *http_bridge_create_provider(sirio_provider provider)
{
    sirio_bridge_config config;
    sirio_bridge *bridge;

    memset(&config, 0, sizeof(config));
    config.api_key = HTTP_SECRET;
    config.provider = provider;
    config.auth_method = provider == SIRIO_PROVIDER_OPENAI ?
                         SIRIO_AUTH_OAUTH : SIRIO_AUTH_API_KEY;
    bridge = sirio_bridge_create(&config);
    if (bridge != NULL && provider == SIRIO_PROVIDER_OPENAI) {
        sirio_generation_options generation = {
            .temperature = -1.0,
            .top_p = -1.0,
            .reasoning = SIRIO_REASONING_HIGH,
        };
        bridge->openai.access_token = bridge_strdup(HTTP_SECRET);
        bridge->openai.account_id = bridge_strdup("acct-http-test");
        bridge->openai.access_expires_at = time(NULL) + 3600;
        if (bridge->openai.access_token == NULL ||
            bridge->openai.account_id == NULL ||
            sirio_bridge_set_generation_options(bridge, &generation) != 0) {
            sirio_bridge_destroy(bridge);
            return NULL;
        }
    }
    return bridge;
}

static sirio_bridge *http_bridge_create(void)
{
    return http_bridge_create_provider(SIRIO_PROVIDER_DEEPSEEK);
}

static sirio_bridge *http_bridge_create_openai_api_key(void)
{
    sirio_bridge_config config = {
        .api_key = HTTP_SECRET,
        .provider = SIRIO_PROVIDER_OPENAI,
        .auth_method = SIRIO_AUTH_API_KEY,
        .model = OPENAI_MODEL,
    };
    sirio_bridge *bridge = sirio_bridge_create(&config);
    if (bridge != NULL) {
        sirio_generation_options generation = {
            .temperature = -1.0,
            .top_p = -1.0,
            .reasoning = SIRIO_REASONING_HIGH,
        };
        if (sirio_bridge_set_generation_options(bridge, &generation) != 0) {
            sirio_bridge_destroy(bridge);
            return NULL;
        }
    }
    return bridge;
}

static sirio_bridge *http_bridge_create_chat(sirio_provider provider,
                                             const char *model,
                                             sirio_reasoning_effort reasoning)
{
    sirio_bridge_config config = {
        .api_key = HTTP_SECRET,
        .provider = provider,
        .auth_method = SIRIO_AUTH_API_KEY,
        .model = model,
    };
    sirio_generation_options generation = {
        .temperature = -1.0,
        .top_p = -1.0,
        .reasoning = reasoning,
    };
    sirio_bridge *bridge = sirio_bridge_create(&config);

    if (bridge != NULL &&
        sirio_bridge_set_generation_options(bridge, &generation) != 0) {
        sirio_bridge_destroy(bridge);
        return NULL;
    }
    return bridge;
}

static int http_generate(sirio_bridge *bridge, http_capture *capture)
{
    sirio_message message;

    memset(&message, 0, sizeof(message));
    memset(capture, 0, sizeof(*capture));
    message.role = SIRIO_ROLE_USER;
    message.content = "hello";
    return sirio_bridge_generate(bridge, &message, 1, NULL, 0,
                                 http_capture_event, capture);
}

static void http_check_private(const sirio_bridge *bridge,
                               const http_capture *capture)
{
    const char *error = sirio_bridge_last_error(bridge);

    HTTP_CHECK(error == NULL || strstr(error, HTTP_SECRET) == NULL);
    HTTP_CHECK(strstr(capture->error, HTTP_SECRET) == NULL);
}

static void test_allocator_failures(void)
{
    sirio_bridge_config config;
    sirio_bridge *bridge;
    bridge_buffer buffer;
    stream_response response;
    char *fragment = NULL;

    memset(&config, 0, sizeof(config));
    config.api_key = HTTP_SECRET;
    config.provider = SIRIO_PROVIDER_DEEPSEEK;
    config.auth_method = SIRIO_AUTH_API_KEY;

    http_allocation_calls = 0;
    http_fail_allocation_after = 0;
    HTTP_CHECK(sirio_bridge_create(&config) == NULL);

    http_allocation_calls = 0;
    http_fail_allocation_after = 1;
    HTTP_CHECK(sirio_bridge_create(&config) == NULL);

    memset(&buffer, 0, sizeof(buffer));
    http_allocation_calls = 0;
    http_fail_allocation_after = 0;
    HTTP_CHECK(buffer_append(&buffer, "x", 1) != 0);
    HTTP_CHECK(buffer.data == NULL);
    buffer_free(&buffer);

    memset(&response, 0, sizeof(response));
    http_allocation_calls = 0;
    http_fail_allocation_after = 0;
    HTTP_CHECK(stream_append_fragment(&response, &fragment, "x",
                                      "tool-call fragment") != 0);
    HTTP_CHECK(fragment == NULL);
    HTTP_CHECK(strstr(response.error, "memory") != NULL);
    stream_response_free(&response);

    http_fail_allocation_after = -1;
    http_allocation_calls = 0;
    bridge = sirio_bridge_create(&config);
    HTTP_CHECK(bridge != NULL);
    sirio_bridge_destroy(bridge);
}

static void http_check_missing_key(const sirio_bridge_config *config)
{
    sirio_bridge *bridge = sirio_bridge_create(config);
    http_capture capture;
    const char *error;

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL &&
               strstr(error, "API key authentication") != NULL);
    HTTP_CHECK(capture.error_events == 1);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static void test_bridge_lifecycle_failures(void)
{
    sirio_bridge_config config;
    sirio_bridge *bridge;
    http_capture capture;
    const char *error;

    memset(&config, 0, sizeof(config));
    HTTP_CHECK(sirio_bridge_create(NULL) == NULL);
    config.provider = SIRIO_PROVIDER_DEEPSEEK;
    config.auth_method = SIRIO_AUTH_API_KEY;
    http_check_missing_key(&config);
    config.api_key = "";
    http_check_missing_key(&config);

    config.api_key = HTTP_SECRET;
    http_fail_global_init = 1;
    bridge = sirio_bridge_create(&config);
    http_fail_global_init = 0;
    HTTP_CHECK(bridge != NULL);
    if (bridge != NULL) {
        error = sirio_bridge_last_error(bridge);
        HTTP_CHECK(error != NULL &&
                   strstr(error, "libcurl initialization failed") != NULL);
        HTTP_CHECK(http_generate(bridge, &capture) != 0);
        HTTP_CHECK(capture.error_events == 1);
        http_check_private(bridge, &capture);
        sirio_bridge_destroy(bridge);
    }

    bridge = sirio_bridge_create(&config);
    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;

    http_fail_easy_init = 1;
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fail_easy_init = 0;
    http_check_private(bridge, &capture);

    http_slist_calls = 0;
    http_fail_slist_after = 0;
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fail_slist_after = -1;
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static void test_success_request_and_privacy(void)
{
    http_fixture fixture;
    http_capture capture;
    sirio_bridge *bridge = http_bridge_create();

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;
    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_sse, 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) == 0);
    http_fixture_join(&fixture);
    HTTP_CHECK(capture.text_events == 1);
    HTTP_CHECK(capture.usage_events == 1);
    HTTP_CHECK(capture.done_events == 1);
    HTTP_CHECK(strstr(fixture.request,
                      "Authorization: Bearer " HTTP_SECRET) != NULL);
    HTTP_CHECK(strstr(fixture.request, "\"stream\":true") != NULL);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static void test_chat_provider_requests(void)
{
    struct {
        sirio_provider provider;
        const char *model;
        sirio_reasoning_effort reasoning;
        const char *reasoning_json;
    } cases[] = {
        {SIRIO_PROVIDER_OPENCODE_GO, OPENCODE_GO_GLM_MODEL,
         SIRIO_REASONING_LOW, "\"reasoning_effort\":\"low\""},
        {SIRIO_PROVIDER_KIMI, KIMI_MODEL,
         SIRIO_REASONING_HIGH, "\"reasoning_effort\":\"high\""},
        {SIRIO_PROVIDER_KIMI, KIMI_CODING_MODEL,
         SIRIO_REASONING_HIGH, NULL},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        http_fixture fixture;
        http_capture capture;
        sirio_bridge *bridge = http_bridge_create_chat(
            cases[i].provider, cases[i].model, cases[i].reasoning);

        HTTP_CHECK(bridge != NULL);
        if (bridge == NULL)
            continue;
        HTTP_FIXTURE_REQUIRE(
            http_fixture_start(
                &fixture, 200, "text/event-stream",
                cases[i].provider == SIRIO_PROVIDER_OPENCODE_GO ?
                    valid_opencode_sse : valid_sse,
                0, 0, 0),
            fixture, bridge);
        HTTP_CHECK(http_generate(bridge, &capture) == 0);
        http_fixture_join(&fixture);
        HTTP_CHECK(strstr(fixture.request,
                          "Authorization: Bearer " HTTP_SECRET) != NULL);
        HTTP_CHECK(strstr(fixture.request, cases[i].model) != NULL);
        HTTP_CHECK(strstr(fixture.request,
                          "\"thinking\":{\"type\":\"enabled\"}") != NULL);
        if (cases[i].reasoning_json != NULL)
            HTTP_CHECK(strstr(fixture.request,
                              cases[i].reasoning_json) != NULL);
        else
            HTTP_CHECK(strstr(fixture.request, "\"reasoning_effort\"") == NULL);
        HTTP_CHECK(capture.done_events == 1);
        http_check_private(bridge, &capture);
        sirio_bridge_destroy(bridge);
    }
}

static void test_http_status_and_content_type_errors(void)
{
    static const char provider_error[] =
        "{\"error\":{\"message\":\"rate limited\"}}";
    http_fixture fixture;
    http_capture capture;
    sirio_bridge *bridge = http_bridge_create();
    const char *error;

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "application/json",
                                            "{}", 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "text/event-stream") != NULL);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 429, "application/json",
                                            provider_error, 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "429") != NULL);
    HTTP_CHECK(error != NULL && strstr(error, "rate limited") != NULL);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 500, "text/plain",
                                            "upstream broke", 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "500") != NULL);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 502, "text/plain", "",
                                            0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "502") != NULL);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 503, "application/json",
                                            "{]", 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "503") != NULL);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

typedef struct {
    sirio_bridge *bridge;
    unsigned delay_ms;
} http_cancel_request;

static void *http_cancel_main(void *private_data)
{
    http_cancel_request *request = private_data;

    http_sleep_ms(request->delay_ms);
    sirio_bridge_cancel(request->bridge);
    return NULL;
}

static void test_transport_timeout_partial_and_cancel(void)
{
    static const char partial_header[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream";
    http_fixture fixture;
    http_capture capture;
    http_cancel_request cancel_request;
    pthread_t cancel_thread;
    sirio_bridge *bridge = http_bridge_create();
    const char *error;

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;

    http_timeout_override = 1;
    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_sse, 0, 1300, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "transport") != NULL);
    http_check_private(bridge, &capture);
    http_timeout_override = -1;

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_sse, 32, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "transport") != NULL);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start_raw(&fixture, partial_header,
                                                sizeof(partial_header) - 1,
                                                0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_sse, 0, 500, 0),
                         fixture, bridge);
    cancel_request.bridge = bridge;
    cancel_request.delay_ms = 50;
    if (pthread_create(&cancel_thread, NULL, http_cancel_main,
                       &cancel_request) != 0) {
        HTTP_CHECK(0);
        http_fixture_abort(&fixture);
        sirio_bridge_destroy(bridge);
        return;
    }
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    (void)pthread_join(cancel_thread, NULL);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "cancelled") != NULL);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static double http_monotonic_seconds(void)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void test_callback_rejection_completion_and_reuse(void)
{
    http_fixture fixture;
    http_capture capture;
    sirio_bridge *bridge = http_bridge_create();
    const char *error;
    double started;
    double elapsed;

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_sse, 0, 0, 0),
                         fixture, bridge);
    memset(&capture, 0, sizeof(capture));
    capture.reject_text = 1;
    {
        sirio_message message = {
            .role = SIRIO_ROLE_USER,
            .content = "hello",
        };
        HTTP_CHECK(sirio_bridge_generate(bridge, &message, 1, NULL, 0,
                                         http_capture_event, &capture) != 0);
    }
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "rejected") != NULL);
    HTTP_CHECK(capture.error_events == 0);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_sse, 0, 0, 1000),
                         fixture, bridge);
    started = http_monotonic_seconds();
    HTTP_CHECK(http_generate(bridge, &capture) == 0);
    elapsed = http_monotonic_seconds() - started;
    HTTP_CHECK(elapsed < 0.75);
    http_fixture_join(&fixture);
    HTTP_CHECK(capture.done_events == 1);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static void test_openai_success_request_and_content_type(void)
{
    http_fixture fixture;
    http_capture capture;
    sirio_bridge *bridge =
        http_bridge_create_provider(SIRIO_PROVIDER_OPENAI);

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;

    /* The Codex transport parses successful bodies as SSE even when an
     * intermediary rewrites or omits the expected content type. */
    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "application/json",
                                            valid_openai_sse, 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) == 0);
    http_fixture_join(&fixture);
    HTTP_CHECK(capture.text_events == 1);
    HTTP_CHECK(capture.usage_events == 1);
    HTTP_CHECK(capture.done_events == 1);
    HTTP_CHECK(strstr(fixture.request,
                      "Authorization: Bearer " HTTP_SECRET) != NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "ChatGPT-Account-ID: acct-http-test") != NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "\"model\":\"" OPENAI_MODEL "\"") != NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "\"reasoning\":{\"effort\":\"high\"") != NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "\"verbosity\":\"low\"") != NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "\"parallel_tool_calls\":false") != NULL);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static void test_openai_api_key_transport(void)
{
    http_fixture fixture;
    http_capture capture;
    sirio_bridge *bridge = http_bridge_create_openai_api_key();

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;
    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_openai_sse, 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) == 0);
    http_fixture_join(&fixture);
    HTTP_CHECK(capture.done_events == 1);
    HTTP_CHECK(strstr(fixture.request,
                      "Authorization: Bearer " HTTP_SECRET) != NULL);
    HTTP_CHECK(strstr(fixture.request, "ChatGPT-Account-ID:") == NULL);
    HTTP_CHECK(strstr(fixture.request, "originator:") == NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "x-openai-internal-codex-responses-lite") == NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "\"model\":\"" OPENAI_MODEL "\"") != NULL);
    HTTP_CHECK(strstr(fixture.request,
                      "\"reasoning\":{\"effort\":\"high\"") != NULL);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static void test_openai_http_errors_and_incomplete_stream(void)
{
    static const char provider_error[] =
        "{\"error\":{\"message\":\"OpenAI rate limited\"}}";
    static const char incomplete_sse[] =
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"unfinished\"}\n\n";
    http_fixture fixture;
    http_capture capture;
    sirio_bridge *bridge =
        http_bridge_create_provider(SIRIO_PROVIDER_OPENAI);
    const char *error;

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 429, "application/json",
                                            provider_error, 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "429") != NULL);
    HTTP_CHECK(error != NULL && strstr(error, "OpenAI rate limited") != NULL);
    HTTP_CHECK(capture.error_events == 1);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            incomplete_sse, 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "without response.completed") != NULL);
    HTTP_CHECK(capture.error_events == 1);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

static void test_openai_timeout_cancel_callback_and_reuse(void)
{
    http_fixture fixture;
    http_capture capture;
    http_cancel_request cancel_request;
    pthread_t cancel_thread;
    sirio_bridge *bridge =
        http_bridge_create_provider(SIRIO_PROVIDER_OPENAI);
    const char *error;

    HTTP_CHECK(bridge != NULL);
    if (bridge == NULL)
        return;

    http_timeout_override = 1;
    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_openai_sse, 0, 1300, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "transport") != NULL);
    http_check_private(bridge, &capture);
    http_timeout_override = -1;

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_openai_sse, 0, 500, 0),
                         fixture, bridge);
    cancel_request.bridge = bridge;
    cancel_request.delay_ms = 50;
    if (pthread_create(&cancel_thread, NULL, http_cancel_main,
                       &cancel_request) != 0) {
        HTTP_CHECK(0);
        http_fixture_abort(&fixture);
        sirio_bridge_destroy(bridge);
        return;
    }
    HTTP_CHECK(http_generate(bridge, &capture) != 0);
    (void)pthread_join(cancel_thread, NULL);
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "cancelled") != NULL);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_openai_sse, 0, 0, 0),
                         fixture, bridge);
    memset(&capture, 0, sizeof(capture));
    capture.reject_text = 1;
    {
        sirio_message message = {
            .role = SIRIO_ROLE_USER,
            .content = "hello",
        };
        HTTP_CHECK(sirio_bridge_generate(bridge, &message, 1, NULL, 0,
                                         http_capture_event, &capture) != 0);
    }
    http_fixture_join(&fixture);
    error = sirio_bridge_last_error(bridge);
    HTTP_CHECK(error != NULL && strstr(error, "rejected") != NULL);
    HTTP_CHECK(capture.error_events == 0);
    http_check_private(bridge, &capture);

    HTTP_FIXTURE_REQUIRE(http_fixture_start(&fixture, 200, "text/event-stream",
                                            valid_openai_sse, 0, 0, 0),
                         fixture, bridge);
    HTTP_CHECK(http_generate(bridge, &capture) == 0);
    http_fixture_join(&fixture);
    HTTP_CHECK(capture.done_events == 1);
    http_check_private(bridge, &capture);
    sirio_bridge_destroy(bridge);
}

int main(void)
{
    static const char *proxy_names[] = {
        "http_proxy", "https_proxy", "all_proxy",
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
    };
    size_t i;

    (void)signal(SIGPIPE, SIG_IGN);
    for (i = 0; i < sizeof(proxy_names) / sizeof(proxy_names[0]); i++)
        (void)unsetenv(proxy_names[i]);
    (void)setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    (void)setenv("no_proxy", "127.0.0.1,localhost", 1);

    test_allocator_failures();
    test_bridge_lifecycle_failures();
    test_success_request_and_privacy();
    test_chat_provider_requests();
    test_http_status_and_content_type_errors();
    test_transport_timeout_partial_and_cancel();
    test_callback_rejection_completion_and_reuse();
    test_openai_success_request_and_content_type();
    test_openai_api_key_transport();
    test_openai_http_errors_and_incomplete_stream();
    test_openai_timeout_cancel_callback_and_reuse();

    if (http_failures != 0) {
        fprintf(stderr, "sirio provider HTTP tests: %d failure(s)\n",
                http_failures);
        return 1;
    }
    puts("sirio provider HTTP tests: ok");
    return 0;
}
