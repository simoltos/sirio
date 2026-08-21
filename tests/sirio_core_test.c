/*
 * Sirio core unit tests (upstream-style unity test main).
 *
 * The parser tests live inside the maintained sirio_core.c guarded by
 * SIRIO_CORE_TEST.  This main includes the core source (exactly like
 * upstream tests/ds4_agent_test.c includes ../ds4_agent.c) so the static
 * test_* functions become reachable, runs the runner, and reports failures.
 *
 * It also regression-tests the adapted worker layer. Production compiles the
 * core and worker separately; this test deliberately includes both sources so
 * their private test seams remain reachable from the upstream-style runner.
 */
#define SIRIO_CORE_TEST
#define SIRIO_CORE_TEST_NO_MAIN
#include <stdatomic.h>
#define sirio_bridge_set_cancel_poll sirio_test_bridge_set_cancel_poll
#include "../sirio_core.c"
#include "../sirio_worker.c"
#undef sirio_bridge_set_cancel_poll

int sirio_main(int argc, char **argv);

static int sirio_test_failures;
static const char *sirio_test_program_path;

#define SIRIO_TEST_ASSERT(cond) do {                                    \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: assertion failed: %s\n",            \
                    __FILE__, __LINE__, #cond);                         \
            sirio_test_failures++;                                      \
        }                                                               \
    } while (0)

void sirio_test_bridge_set_cancel_poll(
        sirio_bridge *bridge, sirio_bridge_cancel_poll poll, void *priv) {
    (void)bridge;
    const char *state = poll != NULL && priv != NULL ? "armed" : "clear";
    if (setenv("SIRIO_TEST_CANCEL_POLL_STATE", state, 1) != 0)
        sirio_test_failures++;
}

static bool sirio_test_expect_signal_handlers;

static bool sirio_test_worker_init_should_fail(int stage) {
    if (sirio_test_expect_signal_handlers) {
        struct sigaction current;
        int status = sigaction(SIGINT, NULL, &current);
        SIRIO_TEST_ASSERT(status == 0);
        if (status == 0)
            SIRIO_TEST_ASSERT(current.sa_handler == agent_sigint_handler);
        status = sigaction(SIGPIPE, NULL, &current);
        SIRIO_TEST_ASSERT(status == 0);
        if (status == 0)
            SIRIO_TEST_ASSERT(current.sa_handler == SIG_IGN);
    }
    const char *value = getenv("SIRIO_TEST_WORKER_INIT_FAIL");
    return value != NULL && atoi(value) == stage;
}

static int fake_compact_generate(
        sirio_bridge *bridge, const sirio_generation_options *options,
        const sirio_message *messages, size_t message_count,
        const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    (void)tools;
    SIRIO_TEST_ASSERT(options != NULL);
    SIRIO_TEST_ASSERT(options->reasoning == SIRIO_REASONING_NONE);
    SIRIO_TEST_ASSERT(options->max_tokens == AGENT_COMPACT_SUMMARY_MAX_TOKENS);
    SIRIO_TEST_ASSERT(tool_count == 0);
    SIRIO_TEST_ASSERT(message_count > 0);
    SIRIO_TEST_ASSERT(messages[message_count - 1].role == SIRIO_ROLE_USER);
    SIRIO_TEST_ASSERT(strstr(messages[message_count - 1].content,
                             "context compaction request") != NULL);
    sirio_bridge_event text = {
        .type = SIRIO_BRIDGE_EVENT_TEXT,
        .text = "durable summary",
    };
    sirio_bridge_event done = {.type = SIRIO_BRIDGE_EVENT_DONE};
    if (callback(&text, private_data) != 0) return -1;
    return callback(&done, private_data);
}

static int fake_compact_failure(
        sirio_bridge *bridge, const sirio_generation_options *options,
        const sirio_message *messages, size_t message_count,
        const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    (void)options;
    (void)messages;
    (void)message_count;
    (void)tools;
    (void)tool_count;
    sirio_bridge_event error = {
        .type = SIRIO_BRIDGE_EVENT_ERROR,
        .error = "synthetic compaction failure",
    };
    callback(&error, private_data);
    return -1;
}

static char fake_long_provider_error[1024];

static int fake_long_error_generate(
        sirio_bridge *bridge, const sirio_message *messages,
        size_t message_count, const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    SIRIO_TEST_ASSERT(messages != NULL);
    SIRIO_TEST_ASSERT(message_count == 1);
    SIRIO_TEST_ASSERT(messages[0].role == SIRIO_ROLE_USER);
    SIRIO_TEST_ASSERT(tools == NULL);
    SIRIO_TEST_ASSERT(tool_count == 0);
    sirio_bridge_event error = {
        .type = SIRIO_BRIDGE_EVENT_ERROR,
        .error = fake_long_provider_error,
    };
    (void)callback(&error, private_data);
    return -1;
}

static int fake_tool_round;
static const char *fake_tool_path;

static int fake_tool_loop_generate(
        sirio_bridge *bridge, const sirio_message *messages,
        size_t message_count, const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    SIRIO_TEST_ASSERT(tools == sirio_native_tools);
    SIRIO_TEST_ASSERT(tool_count == SIRIO_NATIVE_TOOL_COUNT);
    SIRIO_TEST_ASSERT(messages != NULL);

    sirio_bridge_event text = {.type = SIRIO_BRIDGE_EVENT_TEXT};
    char arguments[PATH_MAX + 64];
    if (fake_tool_round == 0) {
        SIRIO_TEST_ASSERT(message_count == 2);
        SIRIO_TEST_ASSERT(messages[0].role == SIRIO_ROLE_SYSTEM);
        SIRIO_TEST_ASSERT(messages[1].role == SIRIO_ROLE_USER);
        SIRIO_TEST_ASSERT(!strcmp(messages[1].content, "inspect fixture"));
        snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}",
                 fake_tool_path);
        sirio_bridge_event reasoning = {
            .type = SIRIO_BRIDGE_EVENT_REASONING,
            .text = "inspect reasoning",
        };
        sirio_bridge_event first_call = {
            .type = SIRIO_BRIDGE_EVENT_TOOL_CALL,
            .tool_call_id = "call-read-1",
            .tool_name = "read",
            .tool_arguments_json = arguments,
        };
        sirio_bridge_event second_call = {
            .type = SIRIO_BRIDGE_EVENT_TOOL_CALL,
            .tool_call_id = "call-read-2",
            .tool_name = "read",
            .tool_arguments_json = arguments,
        };
        if (callback(&reasoning, private_data) != 0 ||
            callback(&first_call, private_data) != 0 ||
            callback(&second_call, private_data) != 0)
            return -1;
    } else {
        SIRIO_TEST_ASSERT(fake_tool_round == 1);
        SIRIO_TEST_ASSERT(message_count == 5);
        SIRIO_TEST_ASSERT(messages[0].role == SIRIO_ROLE_SYSTEM);
        SIRIO_TEST_ASSERT(messages[1].role == SIRIO_ROLE_USER);
        SIRIO_TEST_ASSERT(messages[2].role == SIRIO_ROLE_ASSISTANT);
        SIRIO_TEST_ASSERT(messages[2].reasoning != NULL);
        SIRIO_TEST_ASSERT(!strcmp(messages[2].reasoning,
                                  "inspect reasoning"));
        SIRIO_TEST_ASSERT(messages[2].tool_call_count == 2);
        SIRIO_TEST_ASSERT(!strcmp(messages[2].tool_calls[0].id,
                                  "call-read-1"));
        SIRIO_TEST_ASSERT(!strcmp(messages[2].tool_calls[1].id,
                                  "call-read-2"));
        SIRIO_TEST_ASSERT(messages[3].role == SIRIO_ROLE_TOOL);
        SIRIO_TEST_ASSERT(messages[4].role == SIRIO_ROLE_TOOL);
        SIRIO_TEST_ASSERT(!strcmp(messages[3].tool_call_id,
                                  "call-read-1"));
        SIRIO_TEST_ASSERT(!strcmp(messages[4].tool_call_id,
                                  "call-read-2"));
        SIRIO_TEST_ASSERT(strstr(messages[3].content,
                                 "container runner is unavailable") != NULL);
        SIRIO_TEST_ASSERT(strstr(messages[4].content,
                                 "container runner is unavailable") != NULL);
        text.text = "final answer";
        if (callback(&text, private_data) != 0) return -1;
    }

    sirio_bridge_event usage = {
        .type = SIRIO_BRIDGE_EVENT_USAGE,
        .prompt_tokens = fake_tool_round == 0 ? 20 : 40,
        .completion_tokens = 5,
        .total_tokens = fake_tool_round == 0 ? 25 : 45,
    };
    if (callback(&usage, private_data) != 0) return -1;
    sirio_bridge_event done = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .request_id = fake_tool_round == 0 ? "req-tool-1" : "req-tool-2",
        .model = "deepseek-chat",
        .finish_reason = fake_tool_round == 0 ? "tool_calls" : "stop",
    };
    fake_tool_round++;
    return callback(&done, private_data);
}

static int fake_malformed_recovery_generate(
        sirio_bridge *bridge, const sirio_message *messages,
        size_t message_count, const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    SIRIO_TEST_ASSERT(tools == sirio_native_tools);
    SIRIO_TEST_ASSERT(tool_count == SIRIO_NATIVE_TOOL_COUNT);
    if (fake_tool_round == 0) {
        SIRIO_TEST_ASSERT(message_count == 2);
        sirio_bridge_event reasoning = {
            .type = SIRIO_BRIDGE_EVENT_REASONING,
            .text = "malformed arguments reasoning",
        };
        sirio_bridge_event malformed = {
            .type = SIRIO_BRIDGE_EVENT_TOOL_CALL,
            .tool_call_id = "call-invalid",
            .tool_name = "read",
            .tool_arguments_json = "{\"path\":",
        };
        if (callback(&reasoning, private_data) != 0 ||
            callback(&malformed, private_data) != 0)
            return -1;
    } else {
        SIRIO_TEST_ASSERT(fake_tool_round == 1);
        SIRIO_TEST_ASSERT(message_count == 4);
        SIRIO_TEST_ASSERT(messages[2].role == SIRIO_ROLE_ASSISTANT);
        SIRIO_TEST_ASSERT(messages[2].tool_call_count == 1);
        SIRIO_TEST_ASSERT(messages[3].role == SIRIO_ROLE_TOOL);
        SIRIO_TEST_ASSERT(!strcmp(messages[3].tool_call_id,
                                  "call-invalid"));
        SIRIO_TEST_ASSERT(strstr(messages[3].content,
                                 "Tool error: invalid arguments") != NULL);
        sirio_bridge_event recovered = {
            .type = SIRIO_BRIDGE_EVENT_TEXT,
            .text = "recovered answer",
        };
        if (callback(&recovered, private_data) != 0) return -1;
    }
    sirio_bridge_event usage = {
        .type = SIRIO_BRIDGE_EVENT_USAGE,
        .prompt_tokens = fake_tool_round == 0 ? 12 : 24,
        .completion_tokens = 4,
        .total_tokens = fake_tool_round == 0 ? 16 : 28,
    };
    if (callback(&usage, private_data) != 0) return -1;
    sirio_bridge_event done = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .request_id = fake_tool_round == 0 ? "req-bad" : "req-recovered",
        .model = "deepseek-chat",
        .finish_reason = fake_tool_round == 0 ? "tool_calls" : "stop",
    };
    fake_tool_round++;
    return callback(&done, private_data);
}

static int fake_tool_without_reasoning_generate(
        sirio_bridge *bridge, const sirio_message *messages,
        size_t message_count, const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    SIRIO_TEST_ASSERT(messages != NULL);
    SIRIO_TEST_ASSERT(message_count == 1);
    SIRIO_TEST_ASSERT(tools == sirio_native_tools);
    SIRIO_TEST_ASSERT(tool_count == SIRIO_NATIVE_TOOL_COUNT);
    sirio_bridge_event call = {
        .type = SIRIO_BRIDGE_EVENT_TOOL_CALL,
        .tool_call_id = "call-no-reasoning",
        .tool_name = "read",
        .tool_arguments_json = "{\"path\":\"README.md\"}",
    };
    sirio_bridge_event done = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .request_id = "req-no-reasoning",
        .model = "test-model",
        .finish_reason = "tool_calls",
    };
    if (callback(&call, private_data) != 0) return -1;
    return callback(&done, private_data);
}

static int fake_noninteractive_calls;

static int fake_noninteractive_generate(
        sirio_bridge *bridge, const sirio_message *messages,
        size_t message_count, const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    SIRIO_TEST_ASSERT(messages != NULL);
    SIRIO_TEST_ASSERT(message_count >= 2);
    SIRIO_TEST_ASSERT(messages[0].role == SIRIO_ROLE_SYSTEM);
    SIRIO_TEST_ASSERT(messages[message_count - 1].role == SIRIO_ROLE_USER);
    SIRIO_TEST_ASSERT(!strcmp(messages[message_count - 1].content,
                              "one-shot prompt"));
    SIRIO_TEST_ASSERT(tools == sirio_native_tools);
    SIRIO_TEST_ASSERT(tool_count == SIRIO_NATIVE_TOOL_COUNT);
    fake_noninteractive_calls++;

    sirio_bridge_event text = {
        .type = SIRIO_BRIDGE_EVENT_TEXT,
        .text = "one-shot answer",
    };
    sirio_bridge_event usage = {
        .type = SIRIO_BRIDGE_EVENT_USAGE,
        .prompt_tokens = 12,
        .completion_tokens = 3,
        .total_tokens = 15,
    };
    sirio_bridge_event done = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .request_id = "req-one-shot",
        .model = "deepseek-chat",
        .finish_reason = "stop",
    };
    if (callback(&text, private_data) != 0) return -1;
    if (callback(&usage, private_data) != 0) return -1;
    return callback(&done, private_data);
}

static int fake_stdin_round;
static const char *fake_stdin_tool_path;
static atomic_bool fake_stdin_first_started;

static void sirio_test_sleep_ms(unsigned milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static int fake_stdin_queue_generate(
        sirio_bridge *bridge, const sirio_message *messages,
        size_t message_count, const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    (void)bridge;
    SIRIO_TEST_ASSERT(messages != NULL);
    SIRIO_TEST_ASSERT(tools == sirio_native_tools);
    SIRIO_TEST_ASSERT(tool_count == SIRIO_NATIVE_TOOL_COUNT);
    sirio_bridge_event text = {.type = SIRIO_BRIDGE_EVENT_TEXT};
    char arguments[PATH_MAX + 64];

    if (fake_stdin_round == 0) {
        SIRIO_TEST_ASSERT(message_count >= 2);
        SIRIO_TEST_ASSERT(messages[message_count - 1].role == SIRIO_ROLE_USER);
        SIRIO_TEST_ASSERT(!strcmp(messages[message_count - 1].content,
                                  "first stdin prompt"));
        atomic_store_explicit(&fake_stdin_first_started, true,
                              memory_order_release);
        sirio_test_sleep_ms(300);
        snprintf(arguments, sizeof(arguments), "{\"path\":\"%s\"}",
                 fake_stdin_tool_path);
        sirio_bridge_event reasoning = {
            .type = SIRIO_BRIDGE_EVENT_REASONING,
            .text = "stdin tool reasoning",
        };
        sirio_bridge_event call = {
            .type = SIRIO_BRIDGE_EVENT_TOOL_CALL,
            .tool_call_id = "call-stdin-read",
            .tool_name = "read",
            .tool_arguments_json = arguments,
        };
        if (callback(&reasoning, private_data) != 0 ||
            callback(&call, private_data) != 0)
            return -1;
    } else {
        SIRIO_TEST_ASSERT(fake_stdin_round == 1);
        SIRIO_TEST_ASSERT(message_count >= 6);
        SIRIO_TEST_ASSERT(messages[message_count - 4].role == SIRIO_ROLE_USER);
        SIRIO_TEST_ASSERT(!strcmp(messages[message_count - 4].content,
                                  "first stdin prompt"));
        SIRIO_TEST_ASSERT(messages[message_count - 3].role ==
                          SIRIO_ROLE_ASSISTANT);
        SIRIO_TEST_ASSERT(messages[message_count - 2].role == SIRIO_ROLE_TOOL);
        SIRIO_TEST_ASSERT(!strcmp(messages[message_count - 2].tool_call_id,
                                  "call-stdin-read"));
        SIRIO_TEST_ASSERT(strstr(messages[message_count - 2].content,
                                 "container runner is unavailable") != NULL);
        SIRIO_TEST_ASSERT(messages[message_count - 1].role == SIRIO_ROLE_USER);
        SIRIO_TEST_ASSERT(!strcmp(messages[message_count - 1].content,
                                  "second stdin prompt"));
        text.text = "stdin final answer";
        if (callback(&text, private_data) != 0) return -1;
    }

    sirio_bridge_event usage = {
        .type = SIRIO_BRIDGE_EVENT_USAGE,
        .prompt_tokens = fake_stdin_round == 0 ? 20 : 40,
        .completion_tokens = 4,
        .total_tokens = fake_stdin_round == 0 ? 24 : 44,
    };
    if (callback(&usage, private_data) != 0) return -1;
    sirio_bridge_event done = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .request_id = fake_stdin_round == 0 ? "req-stdin-1" : "req-stdin-2",
        .model = "deepseek-chat",
        .finish_reason = fake_stdin_round == 0 ? "tool_calls" : "stop",
    };
    fake_stdin_round++;
    return callback(&done, private_data);
}

typedef struct {
    int write_fd;
    int error;
} fake_stdin_writer;

static int fake_stdin_write_all(int fd, const char *text) {
    size_t length = strlen(text);
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, text + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        written += (size_t)count;
    }
    return 0;
}

static void *fake_stdin_writer_main(void *private_data) {
    fake_stdin_writer *writer = private_data;
    if (fake_stdin_write_all(writer->write_fd,
                             "first stdin prompt") != 0) {
        writer->error = 1;
        close(writer->write_fd);
        return NULL;
    }
    unsigned waits = 0;
    while (!atomic_load_explicit(&fake_stdin_first_started,
                                 memory_order_acquire) && waits++ < 1000)
        sirio_test_sleep_ms(5);
    if (!atomic_load_explicit(&fake_stdin_first_started,
                              memory_order_acquire) ||
        fake_stdin_write_all(writer->write_fd,
                             "second stdin prompt") != 0)
        writer->error = 1;
    close(writer->write_fd);
    return NULL;
}

static int test_run_noninteractive_stdin(sirio_engine *engine,
                                         agent_config *cfg,
                                         fake_stdin_writer *writer) {
    int saved_stdin = dup(STDIN_FILENO);
    int input_pipe[2] = {-1, -1};
    pthread_t writer_thread;
    bool redirected = false;
    bool writer_started = false;
    int result = -1;

    SIRIO_TEST_ASSERT(saved_stdin >= 0);
    if (saved_stdin < 0) goto cleanup;
    int pipe_status = pipe(input_pipe);
    SIRIO_TEST_ASSERT(pipe_status == 0);
    if (pipe_status != 0) goto cleanup;
    int redirect_status = dup2(input_pipe[0], STDIN_FILENO);
    SIRIO_TEST_ASSERT(redirect_status == STDIN_FILENO);
    if (redirect_status != STDIN_FILENO) goto cleanup;
    redirected = true;
    close(input_pipe[0]);
    input_pipe[0] = -1;
    clearerr(stdin);

    writer->write_fd = input_pipe[1];
    input_pipe[1] = -1;
    int thread_status = pthread_create(&writer_thread, NULL,
                                       fake_stdin_writer_main, writer);
    SIRIO_TEST_ASSERT(thread_status == 0);
    if (thread_status != 0) {
        close(writer->write_fd);
        goto cleanup;
    }
    writer_started = true;
    result = run_agent_non_interactive(engine, cfg);

cleanup:
    if (writer_started)
        SIRIO_TEST_ASSERT(pthread_join(writer_thread, NULL) == 0);
    if (redirected) {
        clearerr(stdin);
        SIRIO_TEST_ASSERT(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    }
    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (input_pipe[1] >= 0) close(input_pipe[1]);
    if (saved_stdin >= 0) close(saved_stdin);
    clearerr(stdin);
    return result;
}

static void *fake_queue_drain_ui(void *private_data) {
    agent_worker *worker = private_data;
    pthread_mutex_lock(&worker->mu);
    while (!worker->queued_user_drain_pending)
        pthread_cond_wait(&worker->cond, &worker->mu);
    pthread_mutex_unlock(&worker->mu);
    worker_answer_queued_user_drain(worker, NULL);
    return NULL;
}

/* Minimal worker fixture: agent_publish*() needs a working mutex and a
 * harmless wake fd; the output lands in w->out for inspection. */
static void test_worker_init(agent_worker *w) {
    memset(w, 0, sizeof(*w));
    w->wake_fd[0] = -1;
    w->wake_fd[1] = -1;
    pthread_mutex_init(&w->mu, NULL);
}

static void test_worker_free(agent_worker *w) {
    free(w->out);
    w->out = NULL;
    w->out_len = w->out_cap = 0;
    pthread_mutex_destroy(&w->mu);
}

static bool test_expect_restoring_session;

static int test_select_model(sirio_engine *engine, const char *selection,
                             const char *reasoning_name,
                             char *error, size_t error_len) {
    if (test_expect_restoring_session)
        SIRIO_TEST_ASSERT(engine->restoring_session);
    const char *name = strrchr(selection, '/');
    sirio_provider provider = SIRIO_PROVIDER_NONE;
    if (name) {
        char provider_name[32];
        size_t length = (size_t)(name - selection);
        if (!length || length >= sizeof(provider_name)) return 2;
        memcpy(provider_name, selection, length);
        provider_name[length] = '\0';
        const sirio_provider_info *info = sirio_provider_find(provider_name);
        if (!info) return 2;
        provider = info->id;
        name++;
    } else {
        name = selection;
    }
    const sirio_model_info *model = provider == SIRIO_PROVIDER_NONE ?
        sirio_model_find(name) : sirio_model_find_for_provider(provider, name);
    sirio_reasoning_effort reasoning = engine->reasoning;
    if (!model) {
        snprintf(error, error_len, "unknown model: %s", selection);
        return 2;
    }
    if (reasoning_name &&
        (!sirio_reasoning_parse(reasoning_name, &reasoning) ||
         !sirio_model_supports_reasoning(model, reasoning))) {
        snprintf(error, error_len, "unsupported reasoning effort");
        return 2;
    }
    engine->provider = model->provider;
    engine->model = model;
    engine->reasoning = reasoning;
    return 0;
}

static bool test_step_model_at_limit;

static int test_step_model(sirio_engine *engine, int direction,
                           bool *at_limit,
                           char *error, size_t error_len) {
    (void)error;
    (void)error_len;
    if (at_limit) *at_limit = test_step_model_at_limit;
    if (test_step_model_at_limit) return 0;
    const char *name = direction < 0 ? "gpt-5.6-luna" : "gpt-5.6-sol";
    engine->model = sirio_model_find_for_provider(
        SIRIO_PROVIDER_OPENAI, name);
    engine->provider = SIRIO_PROVIDER_OPENAI;
    engine->reasoning = direction < 0 ? SIRIO_REASONING_LOW :
                                       SIRIO_REASONING_MAX;
    return 0;
}

static void test_compact_keeps_system_and_tail(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    cfg.gen.ctx_size = 160;
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };
    w.cfg = &cfg;
    w.engine = &engine;
    conv_clear();
    sirio_conv.bridge = engine.bridge;
    conv_append(0, "system prompt");
    conv_append(1, "user one");
    sirio_tool_call call = {
        .id = "compact-call",
        .name = "bash",
        .arguments_json = "{\"command\":\"true\"}",
    };
    conv_append_assistant_native("assistant one", "compact reasoning", NULL,
                                 &call, 1);
    conv_append_tool("compact-call", "ok\n");
    conv_append(1, "user two");
    conv_append(2, "assistant two");
    char err[160] = {0};
    sirio_test_generate_with_options = fake_compact_generate;
    SIRIO_TEST_ASSERT(agent_worker_compact(&w, "test", err, sizeof(err)));
    sirio_test_generate_with_options = NULL;
    SIRIO_TEST_ASSERT(sirio_conv.len == 4);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[0].text, "system prompt"));
    SIRIO_TEST_ASSERT(strstr(sirio_conv.v[1].text,
                             "durable summary") != NULL);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].text, "user two"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[3].text, "assistant two"));
    SIRIO_TEST_ASSERT(strstr(sirio_conv.v[1].text,
                             "context compaction request") == NULL);
    SIRIO_TEST_ASSERT(w.context_used ==
                      (int)transcript_estimate_chars(sirio_conv.chars));
    SIRIO_TEST_ASSERT(w.last_system_prompt_reminder_at == w.context_used);

    conv_free();
    test_worker_free(&w);
}

static void test_compact_failure_preserves_conversation(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };
    w.cfg = &cfg;
    w.engine = &engine;
    conv_clear();
    sirio_conv.bridge = engine.bridge;
    conv_append(0, "system prompt");
    conv_append(1, "user one");
    conv_append(2, "assistant one");
    conv_append(1, "user two");
    transcript_sync(&w);
    int old_chars = sirio_conv.chars;

    char err[160] = {0};
    sirio_test_generate_with_options = fake_compact_failure;
    SIRIO_TEST_ASSERT(!agent_worker_compact(&w, "test", err, sizeof(err)));
    sirio_test_generate_with_options = NULL;
    SIRIO_TEST_ASSERT(strstr(err, "synthetic compaction failure") != NULL);
    SIRIO_TEST_ASSERT(sirio_conv.len == 4);
    SIRIO_TEST_ASSERT(sirio_conv.chars == old_chars);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[0].text, "system prompt"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[1].text, "user one"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].text, "assistant one"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[3].text, "user two"));

    conv_free();
    test_worker_free(&w);
}

static void test_compaction_keeps_a_native_exchange_atomic(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    cfg.gen.ctx_size = 16;
    w.cfg = &cfg;
    conv_clear();
    conv_append(SIRIO_ROLE_SYSTEM, "system");
    conv_append(SIRIO_ROLE_USER, "old user turn");
    sirio_tool_call calls[] = {
        {
            .id = "compact-a",
            .name = "read",
            .arguments_json = "{\"path\":\"a\"}",
        },
        {
            .id = "compact-b",
            .name = "read",
            .arguments_json = "{\"path\":\"b\"}",
        },
    };
    conv_append_assistant_native("", "retained reasoning", NULL, calls, 2);
    conv_append_tool("compact-a", "a");
    conv_append_tool("compact-b", "b");

    int tail = agent_compact_tail_start(&w, sirio_conv.len, 1);
    SIRIO_TEST_ASSERT(tail == 2);
    conv_compact_commit("summary", tail, sirio_conv.len);
    SIRIO_TEST_ASSERT(sirio_conv.len == 5);
    SIRIO_TEST_ASSERT(sirio_conv.v[2].role == SIRIO_ROLE_ASSISTANT);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].reasoning,
                              "retained reasoning"));
    SIRIO_TEST_ASSERT(sirio_conv.v[2].tool_call_count == 2);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].tool_calls[1].id,
                              "compact-b"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[3].tool_call_id, "compact-a"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[4].tool_call_id, "compact-b"));

    conv_free();
    test_worker_free(&w);
}

static void test_long_provider_error_copy_is_bounded(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };
    agent_buf generated = {0};
    worker_gen_ctx context = {
        .w = &w,
        .gen_text = &generated,
        .raw_output = true,
    };
    struct {
        char error[32];
        unsigned char guard[8];
    } result;

    w.cfg = &cfg;
    w.engine = &engine;
    conv_clear();
    sirio_conv.bridge = engine.bridge;
    conv_append(1, "long provider error");
    memset(fake_long_provider_error, 'E',
           sizeof(fake_long_provider_error) - 1);
    fake_long_provider_error[sizeof(fake_long_provider_error) - 1] = '\0';
    memset(&result, 0xa5, sizeof(result));

    sirio_test_generate = fake_long_error_generate;
    SIRIO_TEST_ASSERT(worker_generate(&w, &context, result.error,
                                      sizeof(result.error)) == 1);
    sirio_test_generate = NULL;

    SIRIO_TEST_ASSERT(context.failed);
    SIRIO_TEST_ASSERT(context.err[sizeof(context.err) - 1] == '\0');
    SIRIO_TEST_ASSERT(strlen(context.err) == sizeof(context.err) - 1);
    SIRIO_TEST_ASSERT(result.error[sizeof(result.error) - 1] == '\0');
    SIRIO_TEST_ASSERT(strlen(result.error) == sizeof(result.error) - 1);
    for (size_t i = 0; i < sizeof(result.guard); i++)
        SIRIO_TEST_ASSERT(result.guard[i] == 0xa5);

    agent_buf_release(&generated);
    conv_free();
    test_worker_free(&w);
}

static void test_history_honors_user_turns(void) {
    agent_worker w;
    test_worker_init(&w);
    conv_clear();
    conv_append(0, "system prompt");
    conv_append(1, "user one");
    conv_append(2, "assistant one");
    conv_append(1, "user two");
    conv_append(2, "assistant two");

    char err[160] = {0};
    /* Window of 1 user turn: only the last user/assistant exchange shows,
     * never the system prompt. */
    SIRIO_TEST_ASSERT(agent_worker_show_history(&w, 1, err, sizeof(err)));
    SIRIO_TEST_ASSERT(w.out != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "user two") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "assistant two") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "user one") == NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "system prompt") == NULL);

    /* user_turns <= 0 prints everything, including the system prompt. */
    free(w.out);
    w.out = NULL;
    w.out_len = w.out_cap = 0;
    w.wake_pending = false;
    SIRIO_TEST_ASSERT(agent_worker_show_history(&w, 0, err, sizeof(err)));
    SIRIO_TEST_ASSERT(w.out != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "system prompt") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "user one") != NULL);

    conv_free();
    test_worker_free(&w);
}

static void test_history_formats_assistant_markdown(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_worker_publish_assistant_history(
        &w, "**bold** and `code`", true);
    SIRIO_TEST_ASSERT(w.out != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "assistant: ") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "\x1b[1m") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "\x1b[36m") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "**") == NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "`code`") == NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "bold") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "code") != NULL);
    test_worker_free(&w);
}

static void test_cloud_option_mapping(void) {
    agent_config cfg;
    sirio_config_defaults(&cfg);
    char *argv[] = {
        "sirio", "--non-interactive", "-p", "hello", "--think", "none",
        "--temp", "0.25", "--top-p", "0.75", "-n", "1234",
        "--edit-upto", "--trace", "trace.log",
    };
    char err[256] = {0};
    SIRIO_TEST_ASSERT(sirio_parse_options(
        &cfg, (int)(sizeof(argv) / sizeof(argv[0])), argv,
        err, sizeof(err)) == SIRIO_OPTIONS_OK);
    SIRIO_TEST_ASSERT(cfg.non_interactive);
    SIRIO_TEST_ASSERT(cfg.gen.raw_prompt == false);
    SIRIO_TEST_ASSERT(cfg.gen.n_predict == 1234);
    SIRIO_TEST_ASSERT(cfg.gen.ctx_size == SIRIO_MODEL_CONTEXT_TOKENS);
    SIRIO_TEST_ASSERT(cfg.gen.temperature_set);
    SIRIO_TEST_ASSERT(cfg.gen.top_p_set);
    SIRIO_TEST_ASSERT(cfg.edit_upto);
    SIRIO_TEST_ASSERT(!strcmp(cfg.gen.trace_path, "trace.log"));

    sirio_generation_options generation;
    SIRIO_TEST_ASSERT(sirio_generation_from_config(
        &cfg, &generation, err, sizeof(err)));
    SIRIO_TEST_ASSERT(generation.reasoning == SIRIO_REASONING_NONE);
    SIRIO_TEST_ASSERT(generation.max_tokens == 1234);
    SIRIO_TEST_ASSERT(generation.max_tokens_explicit);
    SIRIO_TEST_ASSERT(generation.temperature == 0.25);
    SIRIO_TEST_ASSERT(generation.top_p == 0.75);
}

static void test_cloud_option_rejections(void) {
    agent_config cfg;
    sirio_config_defaults(&cfg);
    char *thinking_argv[] = {
        "sirio", "--think", "high", "--temp", "0.5"
    };
    char err[256] = {0};
    SIRIO_TEST_ASSERT(sirio_parse_options(
        &cfg, 5, thinking_argv, err, sizeof(err)) == SIRIO_OPTIONS_OK);
    sirio_generation_options generation;
    SIRIO_TEST_ASSERT(sirio_generation_from_config(
        &cfg, &generation, err, sizeof(err)));
    SIRIO_TEST_ASSERT(generation.reasoning == SIRIO_REASONING_HIGH);
    SIRIO_TEST_ASSERT(generation.temperature == 0.5);

    sirio_config_defaults(&cfg);
    char *raw_argv[] = {"sirio", "--raw-prompt", "-p", "hello"};
    SIRIO_TEST_ASSERT(sirio_parse_options(
        &cfg, 4, raw_argv, err, sizeof(err)) == SIRIO_OPTIONS_ERROR);
    SIRIO_TEST_ASSERT(strstr(err, "--non-interactive") != NULL);

    sirio_config_defaults(&cfg);
    char *seed_argv[] = {"sirio", "--seed", "0"};
    SIRIO_TEST_ASSERT(sirio_parse_options(
        &cfg, 3, seed_argv, err, sizeof(err)) == SIRIO_OPTIONS_ERROR);
    SIRIO_TEST_ASSERT(strstr(err, "unknown option") != NULL);

    sirio_config_defaults(&cfg);
    char *context_argv[] = {"sirio", "--ctx", "1000001"};
    SIRIO_TEST_ASSERT(sirio_parse_options(
        &cfg, 3, context_argv, err, sizeof(err)) == SIRIO_OPTIONS_ERROR);
    SIRIO_TEST_ASSERT(strstr(err, "unknown option") != NULL);

    sirio_config_defaults(&cfg);
    char *output_argv[] = {"sirio", "--tokens", "384001"};
    SIRIO_TEST_ASSERT(sirio_parse_options(
        &cfg, 3, output_argv, err, sizeof(err)) == SIRIO_OPTIONS_OK);
    SIRIO_TEST_ASSERT(cfg.gen.n_predict == 384001);
}

static void test_system_prompt_order(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    cfg.gen.system = "Additional system instruction.";
    struct sirio_engine engine = {0};
    w.cfg = &cfg;
    w.engine = &engine;
    char err[160] = {0};
    SIRIO_TEST_ASSERT(agent_worker_reset_to_sysprompt(
        &w, err, sizeof(err)));
    SIRIO_TEST_ASSERT(sirio_conv.len == 1);
    SIRIO_TEST_ASSERT(strstr(sirio_conv.v[0].text,
                             sirio_native_prompt_intro) ==
                      sirio_conv.v[0].text);
    const char *extra = strstr(sirio_conv.v[0].text,
                               cfg.gen.system);
    SIRIO_TEST_ASSERT(extra != NULL);
    SIRIO_TEST_ASSERT(extra > sirio_conv.v[0].text);
    const char *identity = strstr(sirio_conv.v[0].text, "You are Sirio");
    SIRIO_TEST_ASSERT(identity == sirio_conv.v[0].text);
    SIRIO_TEST_ASSERT(identity &&
                      strstr(identity + 1, "You are Sirio") == NULL);
    conv_free();
    test_worker_free(&w);
}

static void test_reasoning_is_rendered_and_captured(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_token_renderer renderer = {
        .worker = &w,
        .format_thinking = true,
        .format_markdown = false,
        .use_color = false,
        .last_output_newline = true,
    };
    agent_buf generated = {0};
    worker_gen_ctx context = {
        .w = &w,
        .renderer = &renderer,
        .gen_text = &generated,
    };
    sirio_bridge_event reasoning = {
        .type = SIRIO_BRIDGE_EVENT_REASONING,
        .text = "private reasoning",
    };
    sirio_bridge_event final_text = {
        .type = SIRIO_BRIDGE_EVENT_TEXT,
        .text = "final answer",
    };
    sirio_bridge_event done = {.type = SIRIO_BRIDGE_EVENT_DONE};

    SIRIO_TEST_ASSERT(worker_gen_event(&reasoning, &context) == 0);
    SIRIO_TEST_ASSERT(context.reasoning_open);
    SIRIO_TEST_ASSERT(worker_gen_event(&final_text, &context) == 0);
    SIRIO_TEST_ASSERT(!context.reasoning_open);
    SIRIO_TEST_ASSERT(worker_gen_event(&done, &context) == 0);
    SIRIO_TEST_ASSERT(generated.ptr != NULL);
    SIRIO_TEST_ASSERT(!strcmp(generated.ptr, "final answer"));
    SIRIO_TEST_ASSERT(strstr(generated.ptr, "private reasoning") == NULL);
    SIRIO_TEST_ASSERT(context.reasoning_text.ptr != NULL);
    SIRIO_TEST_ASSERT(!strcmp(context.reasoning_text.ptr,
                              "private reasoning"));

    renderer_finish(&renderer);
    worker_gen_ctx_free(&context);
    agent_buf_release(&generated);
    test_worker_free(&w);
}

static void test_raw_bridge_events_bypass_stream_parser(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_buf generated = {0};
    worker_gen_ctx context = {
        .w = &w,
        .gen_text = &generated,
        .raw_output = true,
    };
    sirio_bridge_event reasoning = {
        .type = SIRIO_BRIDGE_EVENT_REASONING,
        .text = "raw reasoning",
    };
    sirio_bridge_event final_text = {
        .type = SIRIO_BRIDGE_EVENT_TEXT,
        .text = "raw answer",
    };
    sirio_bridge_event done = {.type = SIRIO_BRIDGE_EVENT_DONE};

    SIRIO_TEST_ASSERT(worker_gen_event(&reasoning, &context) == 0);
    SIRIO_TEST_ASSERT(worker_gen_event(&final_text, &context) == 0);
    SIRIO_TEST_ASSERT(worker_gen_event(&done, &context) == 0);
    SIRIO_TEST_ASSERT(generated.ptr != NULL);
    SIRIO_TEST_ASSERT(!strcmp(generated.ptr, "raw answer"));
    SIRIO_TEST_ASSERT(w.out != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "raw reasoning") != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "raw answer") != NULL);

    agent_buf_release(&generated);
    test_worker_free(&w);
}

static void test_cloud_context_injections(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    struct sirio_engine engine = {0};
    w.cfg = &cfg;
    w.engine = &engine;

    conv_clear();
    conv_append(0, "system prompt");
    transcript_sync(&w);
    agent_worker_note_system_prompt_seen(&w);
    agent_worker_maybe_append_datetime_context(&w);
    SIRIO_TEST_ASSERT(sirio_conv.len == 2);
    SIRIO_TEST_ASSERT(sirio_conv.v[1].role == 0);
    SIRIO_TEST_ASSERT(strstr(sirio_conv.v[1].text,
                             "Current local date and time") != NULL);
    agent_worker_maybe_append_datetime_context(&w);
    SIRIO_TEST_ASSERT(sirio_conv.len == 2);

    conv_clear();
    conv_append(0, "system prompt");
    transcript_sync(&w);
    w.last_system_prompt_reminder_at = 1;
    w.context_used = 1 + AGENT_SYSTEM_PROMPT_REMINDER_TOKENS;
    agent_worker_maybe_append_system_prompt_reminder(&w);
    SIRIO_TEST_ASSERT(sirio_conv.len == 2);
    SIRIO_TEST_ASSERT(sirio_conv.v[1].role == 0);
    SIRIO_TEST_ASSERT(strstr(sirio_conv.v[1].text,
                             "System prompt reminder follows") != NULL);
    SIRIO_TEST_ASSERT(strstr(sirio_conv.v[1].text,
                             "You are Sirio") != NULL);
    SIRIO_TEST_ASSERT(w.last_system_prompt_reminder_at == w.context_used);

    conv_free();
    test_worker_free(&w);
}


static void test_native_argument_contract_validation(void) {
    sirio_tool_call native = {
        .id = "call-args",
        .name = "read",
        .arguments_json =
            "{\"path\":\"file.c\",\"start_line\":2,\"whole\":false}",
    };
    agent_tool_call converted = {0};
    char err[160] = {0};
    SIRIO_TEST_ASSERT(worker_native_call_convert(
        &native, &converted, err, sizeof(err)));
    SIRIO_TEST_ASSERT(!strcmp(worker_tool_arg_value(&converted, "path"),
                              "file.c"));
    SIRIO_TEST_ASSERT(!strcmp(worker_tool_arg_value(&converted, "start_line"),
                              "2"));
    worker_tool_call_free(&converted);

    native.arguments_json = "{\"path\":\"file.c\",\"whole\":\"false\"}";
    err[0] = '\0';
    SIRIO_TEST_ASSERT(!worker_native_call_convert(
        &native, &converted, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strstr(err, "must be boolean") != NULL);
    worker_tool_call_free(&converted);

    native.arguments_json = "{\"start_line\":1.5,\"path\":\"file.c\"}";
    err[0] = '\0';
    SIRIO_TEST_ASSERT(!worker_native_call_convert(
        &native, &converted, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strstr(err, "portable integer") != NULL);
    worker_tool_call_free(&converted);

    native.arguments_json = "{\"unknown\":true,\"path\":\"file.c\"}";
    err[0] = '\0';
    SIRIO_TEST_ASSERT(!worker_native_call_convert(
        &native, &converted, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strstr(err, "unknown tool argument") != NULL);
    worker_tool_call_free(&converted);

    native.arguments_json = "{}";
    err[0] = '\0';
    SIRIO_TEST_ASSERT(!worker_native_call_convert(
        &native, &converted, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strstr(err, "missing required") != NULL);
    worker_tool_call_free(&converted);
}

static void test_every_native_tool_has_a_working_portable_schema(void) {
    static const char *valid_arguments[] = {
        "{\"path\":\"x\",\"start_line\":1,\"max_lines\":2,\"whole\":false,\"raw\":true}",
        "{\"count\":3}",
        "{\"path\":\"x\",\"content\":\"y\"}",
        "{\"path\":\"x\",\"old\":\"a\",\"new\":\"b\"}",
        "{\"path\":\".\"}",
        "{\"query\":\"x\",\"path\":\".\",\"mode\":\"literal\",\"glob\":\"*.c\",\"context\":2,\"max_results\":3,\"case_sensitive\":true}",
        "{\"command\":\"true\",\"timeout_sec\":1,\"refresh_sec\":1}",
        "{\"job\":1,\"pid\":2,\"refresh_sec\":1}",
        "{\"job\":1,\"pid\":2,\"refresh_sec\":1}",
        "{\"query\":\"x\"}",
        "{\"url\":\"https://example.com\"}",
        "{\"prompt\":\"delegate\",\"model\":\"dflash\",\"reasoning\":\"none\"}",
    };
    SIRIO_TEST_ASSERT(SIRIO_NATIVE_TOOL_COUNT ==
                      sizeof(valid_arguments) / sizeof(valid_arguments[0]));
    SIRIO_TEST_ASSERT(worker_native_tool_find("subagent") != NULL);
    SIRIO_TEST_ASSERT(worker_native_tool_find("sirio") == NULL);
    for (size_t i = 0; i < SIRIO_NATIVE_TOOL_COUNT; i++) {
        sirio_tool_call native = {
            .id = "schema-check",
            .name = (char *)sirio_native_tools[i].name,
            .arguments_json = (char *)valid_arguments[i],
        };
        agent_tool_call converted = {0};
        char err[160] = {0};
        SIRIO_TEST_ASSERT(worker_native_call_convert(
            &native, &converted, err, sizeof(err)));
        SIRIO_TEST_ASSERT(converted.name != NULL);
        SIRIO_TEST_ASSERT(!strcmp(converted.name,
                                  sirio_native_tools[i].name));
        worker_tool_call_free(&converted);
    }
}

static void test_subagent_tool_runs_on_the_host(void) {
    const char *old_child_env = getenv("SIRIO_TEST_SUBPROCESS_CHILD");
    const char *old_depth_env = getenv(SIRIO_SUBPROCESS_DEPTH_ENV);
    char *old_child = old_child_env ? xstrdup(old_child_env) : NULL;
    char *old_depth = old_depth_env ? xstrdup(old_depth_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_SUBPROCESS_CHILD", "1", 1) == 0);
    SIRIO_TEST_ASSERT(setenv(SIRIO_SUBPROCESS_DEPTH_ENV, "0", 1) == 0);

    agent_worker worker;
    test_worker_init(&worker);
    agent_config config;
    sirio_config_defaults(&config);
    config.executable_path = sirio_test_program_path;
    sirio_engine engine = {
        .provider = SIRIO_PROVIDER_OPENCODE_GO,
        .model = sirio_model_find_for_provider(
            SIRIO_PROVIDER_OPENCODE_GO, "deepseek-v4-flash"),
        .reasoning = SIRIO_REASONING_NONE,
    };
    worker.cfg = &config;
    worker.engine = &engine;

    sirio_tool_call native = {
        .id = "delegate-call",
        .name = "subagent",
        .arguments_json = "{\"prompt\":\"delegated task\"}",
    };
    agent_tool_call call = {0};
    char error[160] = {0};
    SIRIO_TEST_ASSERT(worker_native_call_convert(
        &native, &call, error, sizeof(error)));
    char *result = worker_execute_external_tool(&worker, &call);
    SIRIO_TEST_ASSERT(result != NULL);
    SIRIO_TEST_ASSERT(result && !strcmp(result, "delegated answer\n"));
    SIRIO_TEST_ASSERT(result && strstr(result, "child diagnostic") == NULL);
    free(result);
    worker_tool_call_free(&call);

    native.arguments_json =
        "{\"prompt\":\"specialist\",\"model\":\"openai/gpt-5.6-sol\","
        "\"reasoning\":\"high\"}";
    SIRIO_TEST_ASSERT(worker_native_call_convert(
        &native, &call, error, sizeof(error)));
    result = worker_execute_external_tool(&worker, &call);
    SIRIO_TEST_ASSERT(result && !strcmp(result, "specialist answer\n"));
    free(result);
    worker_tool_call_free(&call);

    native.arguments_json = "{\"prompt\":\"fail\"}";
    SIRIO_TEST_ASSERT(worker_native_call_convert(
        &native, &call, error, sizeof(error)));
    result = worker_execute_external_tool(&worker, &call);
    SIRIO_TEST_ASSERT(result && strstr(result, "status 7") != NULL);
    SIRIO_TEST_ASSERT(result && strstr(result, "child failure") != NULL);
    SIRIO_TEST_ASSERT(result && strstr(result, "partial answer") != NULL);
    free(result);
    worker_tool_call_free(&call);

    SIRIO_TEST_ASSERT(setenv(SIRIO_SUBPROCESS_DEPTH_ENV, "4", 1) == 0);
    native.arguments_json = "{\"prompt\":\"too deep\"}";
    SIRIO_TEST_ASSERT(worker_native_call_convert(
        &native, &call, error, sizeof(error)));
    result = worker_execute_external_tool(&worker, &call);
    SIRIO_TEST_ASSERT(result && strstr(result, "depth limit (4)") != NULL);
    free(result);
    worker_tool_call_free(&call);

    test_worker_free(&worker);
    if (old_child) {
        SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_SUBPROCESS_CHILD",
                                 old_child, 1) == 0);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("SIRIO_TEST_SUBPROCESS_CHILD") == 0);
    }
    if (old_depth) {
        SIRIO_TEST_ASSERT(setenv(SIRIO_SUBPROCESS_DEPTH_ENV,
                                 old_depth, 1) == 0);
    } else {
        SIRIO_TEST_ASSERT(unsetenv(SIRIO_SUBPROCESS_DEPTH_ENV) == 0);
    }
    free(old_child);
    free(old_depth);
}

static void test_thinking_tool_calls_allow_direct_calls(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    w.cfg = &cfg;
    conv_clear();
    sirio_conv.bridge = (sirio_bridge *)(uintptr_t)1;
    conv_append(SIRIO_ROLE_SYSTEM, "system");

    agent_buf generated = {0};
    worker_gen_ctx context = {
        .w = &w,
        .gen_text = &generated,
        .enable_tools = true,
    };
    char err[160] = {0};
    sirio_test_generate = fake_tool_without_reasoning_generate;
    SIRIO_TEST_ASSERT(worker_generate(&w, &context,
                                      err, sizeof(err)) == 0);
    SIRIO_TEST_ASSERT(context.tool_call_count == 1);
    SIRIO_TEST_ASSERT(!context.saw_reasoning);
    SIRIO_TEST_ASSERT(context.provider_state_json.len == 0);
    sirio_test_generate = NULL;
    worker_gen_ctx_free(&context);
    agent_buf_release(&generated);
    conv_free();
    test_worker_free(&w);
}

static void test_provider_usage_anchors_context_accounting(void) {
    agent_worker w;
    test_worker_init(&w);
    conv_clear();
    conv_append(0, "12345678");
    transcript_sync(&w);
    SIRIO_TEST_ASSERT(w.context_used == 2);

    sirio_conv.provider_anchor_chars = sirio_conv.chars;
    sirio_conv.provider_prompt_tokens = 41;
    sirio_conv.provider_usage_valid = true;
    transcript_sync(&w);
    SIRIO_TEST_ASSERT(w.context_used == 41);

    conv_append(1, "abcde");
    transcript_sync(&w);
    SIRIO_TEST_ASSERT(w.context_used == 43);

    agent_buf generated = {0};
    worker_gen_ctx context = {
        .w = &w,
        .gen_text = &generated,
        .raw_output = true,
    };
    sirio_bridge_event usage = {
        .type = SIRIO_BRIDGE_EVENT_USAGE,
        .prompt_tokens = 50,
        .completion_tokens = 8,
        .total_tokens = 58,
        .reasoning_tokens = 3,
        .prompt_cache_hit_tokens = 20,
        .prompt_cache_miss_tokens = 30,
    };
    sirio_bridge_event done = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .request_id = "req-worker",
        .model = "deepseek-chat",
        .finish_reason = "stop",
    };
    SIRIO_TEST_ASSERT(worker_gen_event(&usage, &context) == 0);
    SIRIO_TEST_ASSERT(worker_gen_event(&done, &context) == 0);
    SIRIO_TEST_ASSERT(context.has_usage);
    SIRIO_TEST_ASSERT(context.prompt_tokens == 50);
    SIRIO_TEST_ASSERT(context.reasoning_tokens == 3);
    SIRIO_TEST_ASSERT(context.saw_done);
    SIRIO_TEST_ASSERT(!strcmp(context.request_id, "req-worker"));
    SIRIO_TEST_ASSERT(!strcmp(context.model, "deepseek-chat"));
    SIRIO_TEST_ASSERT(!strcmp(context.finish_reason, "stop"));

    agent_buf_release(&generated);
    conv_free();
    test_worker_free(&w);
}

static void test_native_multi_tool_round_preserves_provider_order(void) {
    char path[] = "/tmp/sirio-tool-round-XXXXXX";
    int fd = mkstemp(path);
    SIRIO_TEST_ASSERT(fd >= 0);
    if (fd < 0) return;
    static const char contents[] = "fixture contents\n";
    SIRIO_TEST_ASSERT(write(fd, contents, sizeof(contents) - 1) ==
                      (ssize_t)(sizeof(contents) - 1));
    SIRIO_TEST_ASSERT(close(fd) == 0);

    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };
    w.cfg = &cfg;
    w.engine = &engine;
    w.initialized = true;
    w.datetime_context_injected = true;
    conv_clear();
    sirio_conv.bridge = engine.bridge;
    conv_append(0, "system prompt");
    transcript_sync(&w);
    agent_worker_note_system_prompt_seen(&w);

    fake_tool_round = 0;
    fake_tool_path = path;
    sirio_test_generate = fake_tool_loop_generate;
    pthread_t ui_thread;
    int thread_status = pthread_create(&ui_thread, NULL,
                                       fake_queue_drain_ui, &w);
    SIRIO_TEST_ASSERT(thread_status == 0);
    if (thread_status != 0) {
        sirio_test_generate = NULL;
        fake_tool_path = NULL;
        unlink(path);
        conv_free();
        test_worker_free(&w);
        return;
    }
    SIRIO_TEST_ASSERT(worker_run_turn(&w, "inspect fixture") == 0);
    SIRIO_TEST_ASSERT(pthread_join(ui_thread, NULL) == 0);
    sirio_test_generate = NULL;
    fake_tool_path = NULL;

    SIRIO_TEST_ASSERT(fake_tool_round == 2);
    SIRIO_TEST_ASSERT(sirio_conv.len == 6);
    SIRIO_TEST_ASSERT(sirio_conv.v[0].role == 0);
    SIRIO_TEST_ASSERT(sirio_conv.v[1].role == 1);
    SIRIO_TEST_ASSERT(sirio_conv.v[2].role == 2);
    SIRIO_TEST_ASSERT(sirio_conv.v[3].role == 3);
    SIRIO_TEST_ASSERT(sirio_conv.v[4].role == 3);
    SIRIO_TEST_ASSERT(sirio_conv.v[5].role == 2);
    SIRIO_TEST_ASSERT(sirio_conv.v[2].tool_call_count == 2);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].reasoning,
                              "inspect reasoning"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[3].tool_call_id,
                              "call-read-1"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[4].tool_call_id,
                              "call-read-2"));
    SIRIO_TEST_ASSERT(strstr(sirio_conv.v[3].text,
                             "container runner is unavailable") != NULL);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[5].text, "final answer"));

    SIRIO_TEST_ASSERT(unlink(path) == 0);
    agent_worker_clear_session_identity(&w);
    conv_free();
    test_worker_free(&w);
}

static void test_invalid_native_arguments_recover_on_next_request(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };
    w.cfg = &cfg;
    w.engine = &engine;
    w.initialized = true;
    w.datetime_context_injected = true;
    conv_clear();
    sirio_conv.bridge = engine.bridge;
    conv_append(0, "system prompt");
    transcript_sync(&w);
    agent_worker_note_system_prompt_seen(&w);

    fake_tool_round = 0;
    sirio_test_generate = fake_malformed_recovery_generate;
    pthread_t ui_thread;
    int thread_status = pthread_create(&ui_thread, NULL,
                                       fake_queue_drain_ui, &w);
    SIRIO_TEST_ASSERT(thread_status == 0);
    if (thread_status == 0) {
        SIRIO_TEST_ASSERT(worker_run_turn(&w, "recover malformed tool") == 0);
        SIRIO_TEST_ASSERT(pthread_join(ui_thread, NULL) == 0);
        SIRIO_TEST_ASSERT(fake_tool_round == 2);
        SIRIO_TEST_ASSERT(sirio_conv.len == 5);
        SIRIO_TEST_ASSERT(sirio_conv.v[3].role == 3);
        SIRIO_TEST_ASSERT(strstr(sirio_conv.v[3].text,
                                 "invalid arguments") != NULL);
        SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[4].text,
                                  "recovered answer"));
    }
    sirio_test_generate = NULL;
    agent_worker_clear_session_identity(&w);
    conv_free();
    test_worker_free(&w);
}

static void test_noninteractive_one_shot_frontend(void) {
    char cache_template[] = "/tmp/sirio-noninteractive-XXXXXX";
    char *cache_root = mkdtemp(cache_template);
    SIRIO_TEST_ASSERT(cache_root != NULL);
    if (cache_root == NULL) return;

    const char *old_home_env = getenv("HOME");
    const char *old_poll_env = getenv("SIRIO_TEST_CANCEL_POLL_STATE");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    char *old_poll = old_poll_env ? xstrdup(old_poll_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", cache_root, 1) == 0);
    SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_CANCEL_POLL_STATE",
                             "unset", 1) == 0);

    agent_config cfg;
    sirio_config_defaults(&cfg);
    cfg.non_interactive = true;
    cfg.gen.prompt = "one-shot prompt";
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };
    fake_noninteractive_calls = 0;
    sirio_test_generate = fake_noninteractive_generate;
    SIRIO_TEST_ASSERT(run_agent_non_interactive(&engine, &cfg) == 0);
    sirio_test_generate = NULL;
    SIRIO_TEST_ASSERT(fake_noninteractive_calls == 1);
    SIRIO_TEST_ASSERT(!strcmp(getenv("SIRIO_TEST_CANCEL_POLL_STATE"),
                              "clear"));

    if (old_home != NULL) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    }
    if (old_poll != NULL) {
        SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_CANCEL_POLL_STATE",
                                 old_poll, 1) == 0);
        free(old_poll);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("SIRIO_TEST_CANCEL_POLL_STATE") == 0);
    }

    char sessions_path[PATH_MAX];
    char sirio_path[PATH_MAX];
    snprintf(sessions_path, sizeof(sessions_path), "%s/.sirio/sessions",
             cache_root);
    snprintf(sirio_path, sizeof(sirio_path), "%s/.sirio", cache_root);
    SIRIO_TEST_ASSERT(rmdir(sessions_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(sirio_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(cache_root) == 0);
}

static void test_noninteractive_stdin_queue_protocol(void) {
    char tool_path[] = "/tmp/sirio-stdin-tool-XXXXXX";
    int tool_fd = mkstemp(tool_path);
    SIRIO_TEST_ASSERT(tool_fd >= 0);
    if (tool_fd < 0) return;
    static const char tool_contents[] = "stdin tool contents\n";
    SIRIO_TEST_ASSERT(write(tool_fd, tool_contents,
                            sizeof(tool_contents) - 1) ==
                      (ssize_t)(sizeof(tool_contents) - 1));
    SIRIO_TEST_ASSERT(close(tool_fd) == 0);

    char cache_template[] = "/tmp/sirio-stdin-cache-XXXXXX";
    char *cache_root = mkdtemp(cache_template);
    SIRIO_TEST_ASSERT(cache_root != NULL);
    if (cache_root == NULL) {
        unlink(tool_path);
        return;
    }
    const char *old_home_env = getenv("HOME");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", cache_root, 1) == 0);

    agent_config cfg;
    sirio_config_defaults(&cfg);
    cfg.non_interactive = true;
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };
    fake_stdin_writer writer = {0};
    fake_stdin_round = 0;
    fake_stdin_tool_path = tool_path;
    atomic_store_explicit(&fake_stdin_first_started, false,
                          memory_order_relaxed);
    sirio_test_generate = fake_stdin_queue_generate;
    SIRIO_TEST_ASSERT(test_run_noninteractive_stdin(&engine, &cfg,
                                                     &writer) == 0);
    sirio_test_generate = NULL;
    fake_stdin_tool_path = NULL;
    SIRIO_TEST_ASSERT(writer.error == 0);
    SIRIO_TEST_ASSERT(fake_stdin_round == 2);

    if (old_home != NULL) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    }
    char sessions_path[PATH_MAX];
    char sirio_path[PATH_MAX];
    snprintf(sessions_path, sizeof(sessions_path), "%s/.sirio/sessions",
             cache_root);
    snprintf(sirio_path, sizeof(sirio_path), "%s/.sirio", cache_root);
    SIRIO_TEST_ASSERT(unlink(tool_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(sessions_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(sirio_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(cache_root) == 0);
}

static void test_cloud_status_omits_local_engine_metrics(void) {
    agent_status status = {
        .state = AGENT_WORKER_GENERATING,
        .generated = 123,
        .gen_tps = 45.0,
        .ctx_used = 1200,
        .ctx_size = 100000,
        .provider = "openai",
        .model = "luna",
        .reasoning = "low",
    };
    char text[256];
    build_status_text(&status, text, sizeof(text));
    SIRIO_TEST_ASSERT(strstr(text, "luna · low") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "openai") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "streaming") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "tokens") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "t/s") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "25%") == NULL);

    status.state = AGENT_WORKER_COMPACTING;
    build_status_text(&status, text, sizeof(text));
    SIRIO_TEST_ASSERT(strstr(text, "compacting") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "t/s") == NULL);

    status.state = AGENT_WORKER_DRAINING;
    build_status_text(&status, text, sizeof(text));
    SIRIO_TEST_ASSERT(strstr(text, "stopping") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "distributed") == NULL);
}

static void test_footer_clips_selection_on_the_right(void) {
    char text[64];
    agent_footer_clip("openai · an-extremely-long-model · medium", 18,
                      text, sizeof(text));
    SIRIO_TEST_ASSERT(strlen(text) >= 3);
    SIRIO_TEST_ASSERT(!strcmp(text + strlen(text) - 3, "..."));
    size_t characters = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if ((*p & 0xc0) != 0x80) characters++;
    SIRIO_TEST_ASSERT(characters <= 18);
    agent_footer_clip("selection", 3, text, sizeof(text));
    SIRIO_TEST_ASSERT(!strcmp(text, "..."));
}

static void test_runtime_selection_and_reasoning_steps(void) {
    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    w.cfg = &cfg;
    w.initialized = true;
    w.status.state = AGENT_WORKER_IDLE;
    struct sirio_engine engine = {
        .provider = SIRIO_PROVIDER_OPENAI,
        .model = sirio_model_find("gpt-5.6-terra"),
        .reasoning = SIRIO_REASONING_MEDIUM,
        .select = test_select_model,
        .step_model = test_step_model,
    };
    w.engine = &engine;
    conv_clear();
    sirio_conv.provider = engine.provider;
    conv_append(0, "system prompt");
    conv_append(1, "user prompt");
    conv_append_assistant_native(
        "answer", "reasoning",
        "[{\"type\":\"reasoning\",\"encrypted_content\":\"opaque\"}]",
        NULL, 0);

    char error[160] = {0};
    bool at_limit = false;
    SIRIO_TEST_ASSERT(agent_worker_step_reasoning(
        &w, -1, &at_limit, error, sizeof(error)));
    SIRIO_TEST_ASSERT(!at_limit);
    SIRIO_TEST_ASSERT(engine.reasoning == SIRIO_REASONING_LOW);
    SIRIO_TEST_ASSERT(sirio_conv.v[sirio_conv.len - 1].role ==
                      SIRIO_ROLE_SYSTEM);
    SIRIO_TEST_ASSERT(strstr(
        sirio_conv.v[sirio_conv.len - 1].text,
        "Current model: openai/gpt-5.6-terra\n"
        "Current reasoning: low") != NULL);
    SIRIO_TEST_ASSERT(w.session_dirty);
    SIRIO_TEST_ASSERT(agent_worker_step_reasoning(
        &w, -1, &at_limit, error, sizeof(error)));
    SIRIO_TEST_ASSERT(at_limit);
    SIRIO_TEST_ASSERT(engine.reasoning == SIRIO_REASONING_LOW);

    error[0] = '\0';
    SIRIO_TEST_ASSERT(agent_worker_select_model(
        &w, "deepseek/deepseek-v4-pro", "max", error, sizeof(error)));
    SIRIO_TEST_ASSERT(engine.provider == SIRIO_PROVIDER_DEEPSEEK);
    SIRIO_TEST_ASSERT(engine.model &&
                      !strcmp(engine.model->name, "deepseek-v4-pro"));
    SIRIO_TEST_ASSERT(sirio_conv.v[2].provider_state_json == NULL);
    SIRIO_TEST_ASSERT(strstr(
        sirio_conv.v[sirio_conv.len - 1].text,
        "Current model: deepseek/deepseek-v4-pro\n"
        "Current reasoning: max") != NULL);

    sirio_conv.v[2].provider_state_json = xstrdup("opaque-again");
    conv_recount_chars();

    error[0] = '\0';
    SIRIO_TEST_ASSERT(agent_worker_select_model(
        &w, "gpt-5.6-sol", "max", error, sizeof(error)));
    SIRIO_TEST_ASSERT(engine.provider == SIRIO_PROVIDER_OPENAI);
    SIRIO_TEST_ASSERT(engine.model &&
                      !strcmp(engine.model->name, "gpt-5.6-sol"));
    SIRIO_TEST_ASSERT(engine.reasoning == SIRIO_REASONING_MAX);
    SIRIO_TEST_ASSERT(sirio_conv.v[2].provider_state_json == NULL);
    SIRIO_TEST_ASSERT(!strcmp(w.status.provider, "openai"));
    SIRIO_TEST_ASSERT(!strcmp(w.status.model, "gpt-5.6-sol"));
    SIRIO_TEST_ASSERT(!strcmp(w.status.reasoning, "max"));

    sirio_conv.v[2].provider_state_json = xstrdup("opaque-step");
    conv_recount_chars();
    test_step_model_at_limit = false;
    SIRIO_TEST_ASSERT(agent_worker_step_model(
        &w, -1, &at_limit, error, sizeof(error)));
    SIRIO_TEST_ASSERT(!at_limit);
    SIRIO_TEST_ASSERT(engine.model &&
                      !strcmp(engine.model->name, "gpt-5.6-luna"));
    SIRIO_TEST_ASSERT(engine.reasoning == SIRIO_REASONING_LOW);
    SIRIO_TEST_ASSERT(sirio_conv.v[2].provider_state_json == NULL);
    SIRIO_TEST_ASSERT(strstr(
        sirio_conv.v[sirio_conv.len - 1].text,
        "Current model: openai/gpt-5.6-luna\n"
        "Current reasoning: low") != NULL);
    test_step_model_at_limit = true;
    SIRIO_TEST_ASSERT(agent_worker_step_model(
        &w, -1, &at_limit, error, sizeof(error)));
    SIRIO_TEST_ASSERT(at_limit);

    pthread_mutex_lock(&w.mu);
    w.status.state = AGENT_WORKER_GENERATING;
    pthread_mutex_unlock(&w.mu);
    error[0] = '\0';
    SIRIO_TEST_ASSERT(!agent_worker_step_model(
        &w, 1, &at_limit, error, sizeof(error)));
    SIRIO_TEST_ASSERT(strstr(error, "idle") != NULL);

    conv_free();
    test_worker_free(&w);
}

static void test_worker_cancel_poll_tracks_interrupt_latch(void) {
    agent_worker w;
    test_worker_init(&w);
    SIRIO_TEST_ASSERT(worker_cancel_poll(&w) == 0);
    pthread_mutex_lock(&w.mu);
    w.interrupt = true;
    pthread_mutex_unlock(&w.mu);
    SIRIO_TEST_ASSERT(worker_cancel_poll(&w) == 1);
    test_worker_free(&w);
}

static void sirio_test_sigint_sentinel(int signal_number) {
    (void)signal_number;
}

typedef struct {
    int status;
    char *out;
    char *err;
} sirio_main_capture;

static char *sirio_test_read_stream(FILE *fp) {
    if (fflush(fp) != 0 || fseek(fp, 0, SEEK_END) != 0) return NULL;
    long end = ftell(fp);
    if (end < 0 || fseek(fp, 0, SEEK_SET) != 0) return NULL;
    char *text = malloc((size_t)end + 1);
    if (!text) return NULL;
    size_t count = fread(text, 1, (size_t)end, fp);
    if (count != (size_t)end) {
        free(text);
        return NULL;
    }
    text[count] = '\0';
    return text;
}

static sirio_main_capture sirio_test_capture_main(
        int argc, char **argv, const char *input) {
    sirio_main_capture capture = {.status = -1};
    FILE *in = tmpfile();
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    int saved_in = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    SIRIO_TEST_ASSERT(in && out && err);
    SIRIO_TEST_ASSERT(saved_in >= 0 && saved_out >= 0 && saved_err >= 0);
    if (!in || !out || !err || saved_in < 0 || saved_out < 0 ||
        saved_err < 0)
        goto done;
    if (input) {
        SIRIO_TEST_ASSERT(fwrite(input, 1, strlen(input), in) == strlen(input));
        rewind(in);
    }
    fflush(NULL);
    SIRIO_TEST_ASSERT(dup2(fileno(in), STDIN_FILENO) == STDIN_FILENO);
    SIRIO_TEST_ASSERT(dup2(fileno(out), STDOUT_FILENO) == STDOUT_FILENO);
    SIRIO_TEST_ASSERT(dup2(fileno(err), STDERR_FILENO) == STDERR_FILENO);
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    capture.status = sirio_main(argc, argv);
    fflush(NULL);
    SIRIO_TEST_ASSERT(dup2(saved_in, STDIN_FILENO) == STDIN_FILENO);
    SIRIO_TEST_ASSERT(dup2(saved_out, STDOUT_FILENO) == STDOUT_FILENO);
    SIRIO_TEST_ASSERT(dup2(saved_err, STDERR_FILENO) == STDERR_FILENO);
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    capture.out = sirio_test_read_stream(out);
    capture.err = sirio_test_read_stream(err);
done:
    if (saved_in >= 0) close(saved_in);
    if (saved_out >= 0) close(saved_out);
    if (saved_err >= 0) close(saved_err);
    if (in) fclose(in);
    if (out) fclose(out);
    if (err) fclose(err);
    return capture;
}

static void sirio_test_capture_free(sirio_main_capture *capture) {
    free(capture->out);
    free(capture->err);
    memset(capture, 0, sizeof(*capture));
}

static const char sirio_test_defaults_json[] =
    "{\n"
    "  \"models\": [\"opencode-go/deepseek-v4-flash\", "
                      "\"opencode-go/deepseek-v4-pro\", "
                      "\"openai/gpt-5.6-luna\"],\n"
    "  \"last_used\": {\"model\": null, \"reasoning\": null}\n"
    "}\n";

static bool sirio_test_write_defaults(const char *home, const char *json) {
    char directory[PATH_MAX];
    char path[PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s/.sirio", home) < 0 ||
        snprintf(path, sizeof(path), "%s/default.json", directory) < 0)
        return false;
    if (mkdir(directory, S_IRWXU) != 0 && errno != EEXIST) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t length = strlen(json);
    bool ok = fwrite(json, 1, length, fp) == length;
    if (fclose(fp) != 0) ok = false;
    if (chmod(path, S_IRUSR | S_IWUSR) != 0) ok = false;
    return ok;
}

static void test_system_prompt_includes_runtime_model_catalog(void) {
    char home_template[] = "/tmp/sirio-system-models-XXXXXX";
    char *home = mkdtemp(home_template);
    SIRIO_TEST_ASSERT(home != NULL);
    if (!home) return;
    SIRIO_TEST_ASSERT(sirio_test_write_defaults(
        home, sirio_test_defaults_json));

    char defaults_path[PATH_MAX];
    snprintf(defaults_path, sizeof(defaults_path),
             "%s/.sirio/default.json", home);
    char error[160] = {0};
    sirio_default_store *defaults = sirio_default_store_load(
        defaults_path, error, sizeof(error));
    SIRIO_TEST_ASSERT(defaults != NULL);
    if (defaults) {
        agent_worker worker;
        test_worker_init(&worker);
        agent_config config;
        sirio_config_defaults(&config);
        sirio_engine engine = {
            .provider = SIRIO_PROVIDER_OPENCODE_GO,
            .model = sirio_model_find_for_provider(
                SIRIO_PROVIDER_OPENCODE_GO, "deepseek-v4-flash"),
            .defaults = defaults,
            .reasoning = SIRIO_REASONING_NONE,
        };
        worker.cfg = &config;
        worker.engine = &engine;

        char *prompt = sirio_build_system_message(&worker);
        SIRIO_TEST_ASSERT(prompt != NULL);
        const char *runtime = prompt ? strstr(
            prompt, "[Runtime model context]") : NULL;
        const char *completion = prompt ? strstr(
            prompt, "Verification and completion:") : NULL;
        SIRIO_TEST_ASSERT(runtime != NULL);
        SIRIO_TEST_ASSERT(completion != NULL && runtime > completion);
        SIRIO_TEST_ASSERT(prompt && strstr(
            prompt,
            "Current model: opencode-go/deepseek-v4-flash\n"
            "Current reasoning: none"));
        SIRIO_TEST_ASSERT(prompt && strstr(
            prompt,
            "- opencode-go/deepseek-v4-flash [current, interface]; "
            "supported reasoning: none, low, high, max; selected: none"));
        SIRIO_TEST_ASSERT(prompt && strstr(
            prompt, "- openai/gpt-5.6-luna [interface]"));
        SIRIO_TEST_ASSERT(prompt && strstr(
            prompt, "- deepseek/deepseek-v4-flash [subagent-only]"));
        SIRIO_TEST_ASSERT(prompt && strstr(
            prompt, "- openai/gpt-5.6-sol [subagent-only]"));
        SIRIO_TEST_ASSERT(prompt && strstr(
            prompt, "- opencode-go/glm-5.3 [subagent-only]"));
        SIRIO_TEST_ASSERT(prompt && strstr(
            prompt, "Calls are synchronous"));
        free(prompt);
        test_worker_free(&worker);
        sirio_default_store_destroy(defaults);
    }

    char sirio_dir[PATH_MAX];
    snprintf(sirio_dir, sizeof(sirio_dir), "%s/.sirio", home);
    SIRIO_TEST_ASSERT(unlink(defaults_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(sirio_dir) == 0);
    SIRIO_TEST_ASSERT(rmdir(home) == 0);
}

static void test_main_parses_options_before_auth(void) {
    char home_template[] = "/tmp/sirio-main-auth-XXXXXX";
    char *home = mkdtemp(home_template);
    SIRIO_TEST_ASSERT(home != NULL);
    if (!home) return;
    const char *old_home_env = getenv("HOME");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", home, 1) == 0);

    char *bad_argv[] = {"sirio", "--not-an-option"};
    char *valid_argv[] = {
        "sirio", "--non-interactive", "--prompt", "hello",
    };
    char *help_argv[] = {"sirio", "--help"};
    sirio_main_capture bad = sirio_test_capture_main(2, bad_argv, NULL);
    SIRIO_TEST_ASSERT(bad.status == 2);
    SIRIO_TEST_ASSERT(bad.err && strstr(bad.err, "unknown option"));
    sirio_test_capture_free(&bad);
    sirio_main_capture valid = sirio_test_capture_main(4, valid_argv, NULL);
    SIRIO_TEST_ASSERT(valid.status == 1);
    SIRIO_TEST_ASSERT(valid.err &&
                      strstr(valid.err,
                             "no interface models are configured") &&
                      strstr(valid.err, "catalog --default"));
    sirio_test_capture_free(&valid);

    char *providers_argv[] = {"sirio", "catalog", "--providers"};
    sirio_main_capture providers = sirio_test_capture_main(
        3, providers_argv, NULL);
    SIRIO_TEST_ASSERT(providers.status == 0);
    for (size_t i = 0; i < sirio_provider_count(); i++) {
        const sirio_provider_info *provider = sirio_provider_at(i);
        SIRIO_TEST_ASSERT(provider && providers.out &&
                          strstr(providers.out, provider->name));
    }
    SIRIO_TEST_ASSERT(!providers.err ||
                      !strstr(providers.err, "models file"));
    sirio_test_capture_free(&providers);

    char *auth_status_argv[] = {"sirio", "auth", "--status"};
    sirio_main_capture auth_status = sirio_test_capture_main(
        3, auth_status_argv, NULL);
    SIRIO_TEST_ASSERT(auth_status.status == 0);
    SIRIO_TEST_ASSERT(auth_status.out && strstr(auth_status.out, "deepseek"));
    SIRIO_TEST_ASSERT(!auth_status.err ||
                      !strstr(auth_status.err, "models file"));
    sirio_test_capture_free(&auth_status);

    char *models_argv[] = {"sirio", "catalog", "--models"};
    sirio_main_capture models = sirio_test_capture_main(
        3, models_argv, NULL);
    SIRIO_TEST_ASSERT(models.status == 0);
    SIRIO_TEST_ASSERT(models.out &&
                      strstr(models.out, "deepseek/deepseek-v4-flash"));
    SIRIO_TEST_ASSERT(models.out && strstr(models.out, "scope subagent"));
    char sirio_dir[PATH_MAX];
    snprintf(sirio_dir, sizeof(sirio_dir), "%s/.sirio", home);
    SIRIO_TEST_ASSERT(access(sirio_dir, F_OK) != 0 && errno == ENOENT);
    sirio_test_capture_free(&models);

    SIRIO_TEST_ASSERT(sirio_test_write_defaults(
        home, sirio_test_defaults_json));
    valid = sirio_test_capture_main(4, valid_argv, NULL);
    SIRIO_TEST_ASSERT(valid.status == 1);
    SIRIO_TEST_ASSERT(valid.err &&
                      strstr(valid.err,
                             "no interface model has configured credentials"));
    sirio_test_capture_free(&valid);
    sirio_main_capture help = sirio_test_capture_main(2, help_argv, NULL);
    SIRIO_TEST_ASSERT(help.status == 0);
    SIRIO_TEST_ASSERT(help.out &&
                      strstr(help.out, "Usage: sirio [command] [options]"));
    sirio_test_capture_free(&help);

    char *bare_auth_argv[] = {"sirio", "auth"};
    sirio_main_capture bare_auth = sirio_test_capture_main(
        2, bare_auth_argv, NULL);
    SIRIO_TEST_ASSERT(bare_auth.status == 2);
    SIRIO_TEST_ASSERT(bare_auth.err &&
                      strstr(bare_auth.err, "sirio auth --help"));
    sirio_test_capture_free(&bare_auth);

    char *auth_help_argv[] = {"sirio", "auth", "--help"};
    sirio_main_capture auth_help = sirio_test_capture_main(
        3, auth_help_argv, NULL);
    SIRIO_TEST_ASSERT(auth_help.status == 0);
    SIRIO_TEST_ASSERT(auth_help.out &&
                      strstr(auth_help.out,
                             "Usage: sirio auth [options]"));
    SIRIO_TEST_ASSERT(auth_help.out &&
                      !strstr(auth_help.out, "Runtime Commands"));
    sirio_test_capture_free(&auth_help);

    char *legacy_argv[] = {"sirio", "--sessions"};
    sirio_main_capture legacy = sirio_test_capture_main(
        2, legacy_argv, NULL);
    SIRIO_TEST_ASSERT(legacy.status == 2);
    SIRIO_TEST_ASSERT(legacy.err && strstr(legacy.err, "unknown option"));
    sirio_test_capture_free(&legacy);

    char *legacy_auth_argv[] = {"sirio", "--api-key", "deepseek"};
    sirio_main_capture legacy_auth = sirio_test_capture_main(
        3, legacy_auth_argv, NULL);
    SIRIO_TEST_ASSERT(legacy_auth.status == 2);
    SIRIO_TEST_ASSERT(legacy_auth.err &&
                      strstr(legacy_auth.err, "unknown option: --api-key"));
    sirio_test_capture_free(&legacy_auth);

    char *catalog_conflict_argv[] = {
        "sirio", "catalog", "--providers", "--models",
    };
    sirio_main_capture catalog_conflict = sirio_test_capture_main(
        4, catalog_conflict_argv, NULL);
    SIRIO_TEST_ASSERT(catalog_conflict.status == 2);
    SIRIO_TEST_ASSERT(catalog_conflict.err &&
                      strstr(catalog_conflict.err, "only one action"));
    sirio_test_capture_free(&catalog_conflict);

    char *sessions_argv[] = {"sirio", "sessions", "--list"};
    sirio_main_capture sessions = sirio_test_capture_main(
        3, sessions_argv, NULL);
    SIRIO_TEST_ASSERT(sessions.status == 0);
    SIRIO_TEST_ASSERT(sessions.out &&
                      strstr(sessions.out, "no saved sessions"));
    sirio_test_capture_free(&sessions);

    char *turns_argv[] = {"sirio", "sessions", "--turns", "3"};
    sirio_main_capture turns = sirio_test_capture_main(
        4, turns_argv, NULL);
    SIRIO_TEST_ASSERT(turns.status == 2);
    SIRIO_TEST_ASSERT(turns.err && strstr(turns.err, "requires --resume"));
    sirio_test_capture_free(&turns);

    char *resume_prompt_argv[] = {
        "sirio", "sessions", "--resume", "a", "--prompt", "hello",
    };
    sirio_main_capture resume_prompt = sirio_test_capture_main(
        6, resume_prompt_argv, NULL);
    SIRIO_TEST_ASSERT(resume_prompt.status == 2);
    SIRIO_TEST_ASSERT(resume_prompt.err &&
                      strstr(resume_prompt.err, "interactive"));
    sirio_test_capture_free(&resume_prompt);

    char *openai_argv[] = {
        "sirio", "--provider", "openai", "--non-interactive",
        "--prompt", "hello",
    };
    sirio_main_capture openai = sirio_test_capture_main(
        6, openai_argv, NULL);
    SIRIO_TEST_ASSERT(openai.status == 2);
    SIRIO_TEST_ASSERT(openai.err &&
                      strstr(openai.err, "--provider is only valid"));
    sirio_test_capture_free(&openai);

    char *root_specialist_argv[] = {
        "sirio", "--model", "openai/gpt-5.6-sol",
        "--non-interactive", "--prompt", "hello",
    };
    sirio_main_capture root_specialist = sirio_test_capture_main(
        6, root_specialist_argv, NULL);
    SIRIO_TEST_ASSERT(root_specialist.status == 2);
    SIRIO_TEST_ASSERT(root_specialist.err &&
                      strstr(root_specialist.err,
                             "available only through subagent"));
    sirio_test_capture_free(&root_specialist);

    SIRIO_TEST_ASSERT(setenv(SIRIO_SUBPROCESS_DEPTH_ENV, "1", 1) == 0);
    sirio_main_capture subprocess_specialist = sirio_test_capture_main(
        6, root_specialist_argv, NULL);
    SIRIO_TEST_ASSERT(subprocess_specialist.status == 1);
    SIRIO_TEST_ASSERT(subprocess_specialist.err &&
                      strstr(subprocess_specialist.err,
                             "no authentication is configured for openai"));
    sirio_test_capture_free(&subprocess_specialist);
    SIRIO_TEST_ASSERT(unsetenv(SIRIO_SUBPROCESS_DEPTH_ENV) == 0);

    if (old_home) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    }
    char defaults_path[PATH_MAX];
    snprintf(defaults_path, sizeof(defaults_path),
             "%s/.sirio/default.json", home);
    SIRIO_TEST_ASSERT(unlink(defaults_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(sirio_dir) == 0);
    SIRIO_TEST_ASSERT(rmdir(home) == 0);
}

static void test_main_manages_interface_defaults(void) {
    char home_template[] = "/tmp/sirio-main-defaults-XXXXXX";
    char *home = mkdtemp(home_template);
    SIRIO_TEST_ASSERT(home != NULL);
    if (!home) return;
    const char *old_home_env = getenv("HOME");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", home, 1) == 0);

    char sirio_dir[PATH_MAX];
    char defaults_path[PATH_MAX];
    char old_models_path[PATH_MAX];
    snprintf(sirio_dir, sizeof(sirio_dir), "%s/.sirio", home);
    snprintf(defaults_path, sizeof(defaults_path),
             "%s/.sirio/default.json", home);
    snprintf(old_models_path, sizeof(old_models_path),
             "%s/.sirio/models.json", home);

    char *remove_missing_argv[] = {
        "sirio", "catalog", "--remove", "retired/old-model",
    };
    sirio_main_capture remove_missing = sirio_test_capture_main(
        4, remove_missing_argv, NULL);
    SIRIO_TEST_ASSERT(remove_missing.status == 0);
    SIRIO_TEST_ASSERT(remove_missing.out &&
                      strstr(remove_missing.out, "unchanged"));
    SIRIO_TEST_ASSERT(access(sirio_dir, F_OK) != 0 && errno == ENOENT);
    sirio_test_capture_free(&remove_missing);

    char *add_argv[] = {
        "sirio", "catalog", "--default",
        "opencode-go/deepseek-v4-flash, openai/gpt-5.6-luna,"
        "opencode-go/deepseek-v4-flash",
    };
    sirio_main_capture add = sirio_test_capture_main(4, add_argv, NULL);
    SIRIO_TEST_ASSERT(add.status == 0);
    SIRIO_TEST_ASSERT(add.out && strstr(add.out, "Added 2"));
    sirio_test_capture_free(&add);

    struct stat directory_stat;
    struct stat file_stat;
    SIRIO_TEST_ASSERT(stat(sirio_dir, &directory_stat) == 0);
    SIRIO_TEST_ASSERT(stat(defaults_path, &file_stat) == 0);
    SIRIO_TEST_ASSERT((directory_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    SIRIO_TEST_ASSERT((file_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    char error[256] = {0};
    sirio_default_store *defaults = sirio_default_store_load(
        defaults_path, error, sizeof(error));
    SIRIO_TEST_ASSERT(defaults != NULL);
    if (defaults) {
        SIRIO_TEST_ASSERT(sirio_default_store_count(defaults) == 2);
        const sirio_model_info *first = sirio_default_store_at(defaults, 0);
        const sirio_model_info *second = sirio_default_store_at(defaults, 1);
        SIRIO_TEST_ASSERT(first && first->provider ==
                          SIRIO_PROVIDER_OPENCODE_GO);
        SIRIO_TEST_ASSERT(second && second->provider == SIRIO_PROVIDER_OPENAI);
        SIRIO_TEST_ASSERT(!sirio_default_store_last_used(
            defaults, NULL, NULL));
        sirio_default_store_destroy(defaults);
    }

    FILE *old_models = fopen(old_models_path, "wb");
    SIRIO_TEST_ASSERT(old_models != NULL);
    if (old_models) {
        fputs("this file is deliberately ignored\n", old_models);
        SIRIO_TEST_ASSERT(fclose(old_models) == 0);
    }
    char *list_argv[] = {"sirio", "catalog", "--models"};
    sirio_main_capture list = sirio_test_capture_main(3, list_argv, NULL);
    SIRIO_TEST_ASSERT(list.status == 0);
    SIRIO_TEST_ASSERT(list.out && strstr(
        list.out, "opencode-go/deepseek-v4-flash"));
    SIRIO_TEST_ASSERT(list.out && strstr(list.out, "scope interface"));
    SIRIO_TEST_ASSERT(list.out && strstr(
        list.out, "opencode-go/glm-5.3"));
    SIRIO_TEST_ASSERT(list.out && strstr(list.out, "scope subagent"));
    sirio_test_capture_free(&list);

    char *invalid_batch_argv[] = {
        "sirio", "catalog", "--default",
        "deepseek/deepseek-v4-pro,missing/no-model",
    };
    sirio_main_capture invalid_batch = sirio_test_capture_main(
        4, invalid_batch_argv, NULL);
    SIRIO_TEST_ASSERT(invalid_batch.status == 2);
    SIRIO_TEST_ASSERT(invalid_batch.err &&
                      strstr(invalid_batch.err, "unknown model provider"));
    sirio_test_capture_free(&invalid_batch);
    defaults = sirio_default_store_load(
        defaults_path, error, sizeof(error));
    SIRIO_TEST_ASSERT(defaults != NULL);
    if (defaults) {
        SIRIO_TEST_ASSERT(sirio_default_store_count(defaults) == 2);
        SIRIO_TEST_ASSERT(!sirio_default_store_contains(
            defaults, sirio_model_find_for_provider(
                SIRIO_PROVIDER_DEEPSEEK, "deepseek-v4-pro")));
        sirio_default_store_destroy(defaults);
    }

    char *short_name_argv[] = {
        "sirio", "catalog", "--default", "gpt-5.6-luna",
    };
    sirio_main_capture short_name = sirio_test_capture_main(
        4, short_name_argv, NULL);
    SIRIO_TEST_ASSERT(short_name.status == 2);
    SIRIO_TEST_ASSERT(short_name.err &&
                      strstr(short_name.err, "provider/model"));
    sirio_test_capture_free(&short_name);

    char *remove_argv[] = {
        "sirio", "catalog", "--remove",
        "opencode-go/deepseek-v4-flash,openai/gpt-5.6-luna",
    };
    sirio_main_capture remove = sirio_test_capture_main(
        4, remove_argv, NULL);
    SIRIO_TEST_ASSERT(remove.status == 0);
    SIRIO_TEST_ASSERT(remove.out && strstr(remove.out, "Removed 2"));
    sirio_test_capture_free(&remove);
    defaults = sirio_default_store_load(
        defaults_path, error, sizeof(error));
    SIRIO_TEST_ASSERT(defaults != NULL);
    if (defaults) {
        SIRIO_TEST_ASSERT(sirio_default_store_count(defaults) == 0);
        sirio_default_store_destroy(defaults);
    }
    SIRIO_TEST_ASSERT(unlink(defaults_path) == 0);
    list = sirio_test_capture_main(3, list_argv, NULL);
    SIRIO_TEST_ASSERT(list.status == 0);
    SIRIO_TEST_ASSERT(list.out && strstr(list.out, "scope subagent"));
    SIRIO_TEST_ASSERT(access(defaults_path, F_OK) != 0 && errno == ENOENT);
    sirio_test_capture_free(&list);

    char *run_argv[] = {
        "sirio", "--non-interactive", "--prompt", "hello",
    };
    sirio_main_capture run = sirio_test_capture_main(4, run_argv, NULL);
    SIRIO_TEST_ASSERT(run.status == 1);
    SIRIO_TEST_ASSERT(run.err && strstr(run.err, "catalog --default"));
    sirio_test_capture_free(&run);

    FILE *broken = fopen(defaults_path, "wb");
    SIRIO_TEST_ASSERT(broken != NULL);
    if (broken) {
        fputs("broken defaults", broken);
        SIRIO_TEST_ASSERT(fclose(broken) == 0);
        SIRIO_TEST_ASSERT(chmod(defaults_path, S_IRUSR | S_IWUSR) == 0);
    }
    list = sirio_test_capture_main(3, list_argv, NULL);
    SIRIO_TEST_ASSERT(list.status == 1);
    SIRIO_TEST_ASSERT(!list.out || !list.out[0]);
    SIRIO_TEST_ASSERT(list.err && strstr(list.err, "invalid default file"));
    sirio_test_capture_free(&list);

    SIRIO_TEST_ASSERT(unlink(old_models_path) == 0);
    SIRIO_TEST_ASSERT(unlink(defaults_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(sirio_dir) == 0);
    if (old_home) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    }
    SIRIO_TEST_ASSERT(rmdir(home) == 0);
}

static void test_main_auth_and_selection_actions(void) {
    static const char deep_secret[] = "deep-secret-for-test";
    static const char openai_secret[] = "openai-secret-for-test";
    static const char opencode_secret[] = "opencode-secret-for-test";
    char home_template[] = "/tmp/sirio-main-actions-XXXXXX";
    char *home = mkdtemp(home_template);
    SIRIO_TEST_ASSERT(home != NULL);
    if (!home) return;

    const char *old_home_env = getenv("HOME");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", home, 1) == 0);
    SIRIO_TEST_ASSERT(sirio_test_write_defaults(
        home, sirio_test_defaults_json));

    char *deep_argv[] = {
        "sirio", "auth", "--api-key", "deepseek", "--stdin",
    };
    char deep_input[64];
    snprintf(deep_input, sizeof(deep_input), "%s\n", deep_secret);
    sirio_main_capture deep = sirio_test_capture_main(
        5, deep_argv, deep_input);
    SIRIO_TEST_ASSERT(deep.status == 0);
    SIRIO_TEST_ASSERT(deep.out && !strstr(deep.out, deep_secret));
    SIRIO_TEST_ASSERT(deep.err && !strstr(deep.err, deep_secret));
    sirio_test_capture_free(&deep);

    char *openai_argv[] = {
        "sirio", "auth", "--api-key", "openai", "--stdin",
    };
    char openai_input[64];
    snprintf(openai_input, sizeof(openai_input), "%s\n", openai_secret);
    sirio_main_capture openai = sirio_test_capture_main(
        5, openai_argv, openai_input);
    SIRIO_TEST_ASSERT(openai.status == 0);
    SIRIO_TEST_ASSERT(openai.out && !strstr(openai.out, openai_secret));
    SIRIO_TEST_ASSERT(openai.err && !strstr(openai.err, openai_secret));
    sirio_test_capture_free(&openai);

    char *opencode_argv[] = {
        "sirio", "auth", "--api-key", "opencode-go", "--stdin",
    };
    char opencode_input[64];
    snprintf(opencode_input, sizeof(opencode_input), "%s\n", opencode_secret);
    sirio_main_capture opencode = sirio_test_capture_main(
        5, opencode_argv, opencode_input);
    SIRIO_TEST_ASSERT(opencode.status == 0);
    SIRIO_TEST_ASSERT(opencode.out && !strstr(opencode.out, opencode_secret));
    SIRIO_TEST_ASSERT(opencode.err && !strstr(opencode.err, opencode_secret));
    sirio_test_capture_free(&opencode);

    char auth_path[PATH_MAX];
    snprintf(auth_path, sizeof(auth_path), "%s/.sirio/auth.json", home);
    struct stat st;
    SIRIO_TEST_ASSERT(stat(auth_path, &st) == 0);
    SIRIO_TEST_ASSERT((st.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    char auth_error[256] = {0};
    sirio_auth_store *store = sirio_auth_store_load(
        auth_path, auth_error, sizeof(auth_error));
    SIRIO_TEST_ASSERT(store != NULL);
    if (store) {
        SIRIO_TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_DEEPSEEK, SIRIO_AUTH_API_KEY));
        SIRIO_TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_OPENAI, SIRIO_AUTH_API_KEY));
        SIRIO_TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_OPENCODE_GO, SIRIO_AUTH_API_KEY));
        SIRIO_TEST_ASSERT(sirio_auth_store_preferred(
            store, SIRIO_PROVIDER_OPENAI) == SIRIO_AUTH_API_KEY);
        sirio_auth_store_destroy(store);
    }

    char *status_argv[] = {"sirio", "auth", "--status"};
    sirio_main_capture status = sirio_test_capture_main(
        3, status_argv, NULL);
    SIRIO_TEST_ASSERT(status.status == 0);
    SIRIO_TEST_ASSERT(status.out && strstr(status.out, "deepseek"));
    SIRIO_TEST_ASSERT(status.out && strstr(status.out, "openai"));
    SIRIO_TEST_ASSERT(!strstr(status.out, deep_secret));
    SIRIO_TEST_ASSERT(!strstr(status.out, openai_secret));
    SIRIO_TEST_ASSERT(!strstr(status.out, opencode_secret));
    sirio_test_capture_free(&status);

    char *providers_argv[] = {"sirio", "catalog", "--providers"};
    sirio_main_capture providers = sirio_test_capture_main(
        3, providers_argv, NULL);
    SIRIO_TEST_ASSERT(providers.status == 0);
    SIRIO_TEST_ASSERT(providers.out && strstr(providers.out, "deepseek"));
    SIRIO_TEST_ASSERT(providers.out && strstr(providers.out, "openai"));
    SIRIO_TEST_ASSERT(providers.out && strstr(
        providers.out, "auth api-key; credentials available"));
    sirio_test_capture_free(&providers);

    char *models_argv[] = {
        "sirio", "catalog", "--models", "--provider", "openai",
    };
    sirio_main_capture models = sirio_test_capture_main(
        5, models_argv, NULL);
    SIRIO_TEST_ASSERT(models.status == 0);
    SIRIO_TEST_ASSERT(models.out && strstr(models.out, "gpt-5.6-luna"));
    SIRIO_TEST_ASSERT(models.out && strstr(models.out, "scope interface"));
    SIRIO_TEST_ASSERT(models.out && strstr(models.out, "scope subagent"));
    SIRIO_TEST_ASSERT(!strstr(models.out, "deepseek-v4-flash"));
    sirio_test_capture_free(&models);

    char *interface_models_argv[] = {
        "sirio", "catalog", "--models", "--provider", "opencode-go",
    };
    sirio_main_capture interface_models = sirio_test_capture_main(
        5, interface_models_argv, NULL);
    SIRIO_TEST_ASSERT(interface_models.status == 0);
    SIRIO_TEST_ASSERT(interface_models.out &&
                      strstr(interface_models.out, "deepseek-v4-flash"));
    SIRIO_TEST_ASSERT(interface_models.out &&
                      strstr(interface_models.out, "scope interface"));
    SIRIO_TEST_ASSERT(interface_models.out &&
                      strstr(interface_models.out, "glm-5.3"));
    SIRIO_TEST_ASSERT(interface_models.out &&
                      strstr(interface_models.out, "scope subagent"));
    sirio_test_capture_free(&interface_models);

    char *models_global_first_argv[] = {
        "sirio", "--provider", "openai", "catalog", "--models",
    };
    sirio_main_capture models_global_first = sirio_test_capture_main(
        5, models_global_first_argv, NULL);
    SIRIO_TEST_ASSERT(models_global_first.status == 0);
    SIRIO_TEST_ASSERT(models_global_first.out &&
                      strstr(models_global_first.out, "gpt-5.6-luna"));
    SIRIO_TEST_ASSERT(models_global_first.out &&
                      !strstr(models_global_first.out,
                              "deepseek-v4-flash"));
    sirio_test_capture_free(&models_global_first);

    char defaults_path[PATH_MAX];
    snprintf(defaults_path, sizeof(defaults_path),
             "%s/.sirio/default.json", home);
    SIRIO_TEST_ASSERT(access(defaults_path, F_OK) == 0);
    sirio_default_store *default_store = sirio_default_store_load(
        defaults_path, auth_error, sizeof(auth_error));
    SIRIO_TEST_ASSERT(default_store != NULL);
    if (default_store) {
        const sirio_model_info *selected = sirio_default_store_resolve(
            default_store, "openai/gpt-5.6-luna",
            auth_error, sizeof(auth_error));
        SIRIO_TEST_ASSERT(selected != NULL);
        SIRIO_TEST_ASSERT(sirio_default_store_set_last_used(
            default_store, selected, SIRIO_REASONING_LOW) == 0);
        SIRIO_TEST_ASSERT(sirio_default_store_save(
            default_store, defaults_path,
            auth_error, sizeof(auth_error)) == 0);
        sirio_default_store_destroy(default_store);
    }
    default_store = sirio_default_store_load(
        defaults_path, auth_error, sizeof(auth_error));
    SIRIO_TEST_ASSERT(default_store != NULL);
    if (default_store) {
        const sirio_model_info *last_model = NULL;
        sirio_reasoning_effort last_reasoning = SIRIO_REASONING_NONE;
        SIRIO_TEST_ASSERT(sirio_default_store_last_used(
            default_store, &last_model, &last_reasoning));
        SIRIO_TEST_ASSERT(last_model &&
                          !strcmp(last_model->name, "gpt-5.6-luna"));
        SIRIO_TEST_ASSERT(last_reasoning == SIRIO_REASONING_LOW);
        sirio_default_store_destroy(default_store);
    }

    char *conflict_argv[] = {
        "sirio", "--model", "openai/gpt-5.6-sol",
    };
    sirio_main_capture conflict = sirio_test_capture_main(
        3, conflict_argv, NULL);
    SIRIO_TEST_ASSERT(conflict.status == 2);
    SIRIO_TEST_ASSERT(conflict.err &&
                      strstr(conflict.err, "available only through subagent"));
    sirio_test_capture_free(&conflict);

    char *glm_argv[] = {"sirio", "--model", "glm-5.3"};
    sirio_main_capture glm = sirio_test_capture_main(3, glm_argv, NULL);
    SIRIO_TEST_ASSERT(glm.status == 2);
    SIRIO_TEST_ASSERT(glm.err &&
                      strstr(glm.err, "available only through subagent"));
    sirio_test_capture_free(&glm);

    char *bad_stdin_argv[] = {
        "sirio", "auth", "--api-key", "openai", "--stdin", "--yes",
    };
    sirio_main_capture bad_stdin = sirio_test_capture_main(
        6, bad_stdin_argv, "first\nsecond\n");
    SIRIO_TEST_ASSERT(bad_stdin.status == 1);
    SIRIO_TEST_ASSERT(bad_stdin.err && strstr(bad_stdin.err, "exactly one"));
    sirio_test_capture_free(&bad_stdin);

    char *logout_argv[] = {
        "sirio", "auth", "--logout", "openai", "--yes",
    };
    sirio_main_capture logout = sirio_test_capture_main(
        5, logout_argv, NULL);
    SIRIO_TEST_ASSERT(logout.status == 0);
    sirio_test_capture_free(&logout);
    store = sirio_auth_store_load(auth_path, auth_error, sizeof(auth_error));
    SIRIO_TEST_ASSERT(store != NULL);
    if (store) {
        SIRIO_TEST_ASSERT(!sirio_auth_store_has(
            store, SIRIO_PROVIDER_OPENAI, SIRIO_AUTH_API_KEY));
        SIRIO_TEST_ASSERT(sirio_auth_store_has(
            store, SIRIO_PROVIDER_DEEPSEEK, SIRIO_AUTH_API_KEY));
        sirio_auth_store_destroy(store);
    }

    char *login_argv[] = {"sirio", "auth", "--login", "deepseek"};
    sirio_main_capture login = sirio_test_capture_main(
        4, login_argv, NULL);
    SIRIO_TEST_ASSERT(login.status == 2);
    SIRIO_TEST_ASSERT(login.err && strstr(login.err, "does not support OAuth"));
    sirio_test_capture_free(&login);

    SIRIO_TEST_ASSERT(unlink(auth_path) == 0);
    SIRIO_TEST_ASSERT(unlink(defaults_path) == 0);
    char sirio_dir[PATH_MAX];
    snprintf(sirio_dir, sizeof(sirio_dir), "%s/.sirio", home);
    SIRIO_TEST_ASSERT(rmdir(sirio_dir) == 0);
    if (old_home) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    SIRIO_TEST_ASSERT(rmdir(home) == 0);
}

static void test_main_resume_accepts_supported_session_model(void) {
    char home_template[] = "/tmp/sirio-main-resume-model-XXXXXX";
    char *home = mkdtemp(home_template);
    SIRIO_TEST_ASSERT(home != NULL);
    if (!home) return;
    const char *old_home_env = getenv("HOME");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", home, 1) == 0);

    char sessions_dir[PATH_MAX];
    snprintf(sessions_dir, sizeof(sessions_dir),
             "%s/.sirio/sessions", home);
    SIRIO_TEST_ASSERT(agent_mkdir_p(sessions_dir));
    sirio_conv_msg messages[2] = {
        {.role = SIRIO_ROLE_SYSTEM, .text = "system"},
        {.role = SIRIO_ROLE_USER, .text = "resume specialist"},
    };
    char sha[41];
    agent_session_identity_sha("resume specialist", 100, sha);
    char *path = sirio_session_path(sessions_dir, sha);
    char error[160] = {0};
    SIRIO_TEST_ASSERT(sirio_session_write(
        path, "resume specialist", 100, 101, "openai", "gpt-5.6-sol",
        "high", messages, 2, error, sizeof(error)));

    char *resume_argv[] = {"sirio", "sessions", "--resume", sha};
    sirio_main_capture resume = sirio_test_capture_main(
        4, resume_argv, NULL);
    SIRIO_TEST_ASSERT(resume.status == 1);
    SIRIO_TEST_ASSERT(resume.err && strstr(
        resume.err, "no authentication is configured for openai"));
    SIRIO_TEST_ASSERT(!strstr(resume.err, "only through subagent"));
    sirio_test_capture_free(&resume);

    char stale_sha[41];
    agent_session_identity_sha("stale specialist", 200, stale_sha);
    char *stale_path = sirio_session_path(sessions_dir, stale_sha);
    SIRIO_TEST_ASSERT(sirio_session_write(
        stale_path, "stale specialist", 200, 201, "openai", "retired-model",
        "high", messages, 2, error, sizeof(error)));
    char *stale_argv[] = {"sirio", "sessions", "--resume", stale_sha};
    sirio_main_capture stale = sirio_test_capture_main(
        4, stale_argv, NULL);
    SIRIO_TEST_ASSERT(stale.status == 1);
    SIRIO_TEST_ASSERT(stale.err && strstr(stale.err, "not supported"));
    sirio_test_capture_free(&stale);

    SIRIO_TEST_ASSERT(unlink(path) == 0);
    SIRIO_TEST_ASSERT(unlink(stale_path) == 0);
    free(path);
    free(stale_path);
    SIRIO_TEST_ASSERT(rmdir(sessions_dir) == 0);
    char sirio_dir[PATH_MAX];
    snprintf(sirio_dir, sizeof(sirio_dir), "%s/.sirio", home);
    SIRIO_TEST_ASSERT(rmdir(sirio_dir) == 0);
    if (old_home) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    }
    SIRIO_TEST_ASSERT(rmdir(home) == 0);
}

static void test_agent_entry_installs_and_restores_signals(void) {
    struct sigaction old_int;
    struct sigaction old_pipe;
    struct sigaction sentinel_action;
    struct sigaction current_action;
    memset(&sentinel_action, 0, sizeof(sentinel_action));
    sigemptyset(&sentinel_action.sa_mask);
    sentinel_action.sa_handler = sirio_test_sigint_sentinel;
    int int_status = sigaction(SIGINT, &sentinel_action, &old_int);
    SIRIO_TEST_ASSERT(int_status == 0);
    if (int_status != 0) return;
    int pipe_status = sigaction(SIGPIPE, &sentinel_action, &old_pipe);
    SIRIO_TEST_ASSERT(pipe_status == 0);
    if (pipe_status != 0) {
        SIRIO_TEST_ASSERT(sigaction(SIGINT, &old_int, NULL) == 0);
        return;
    }

    sirio_bridge_config bridge_config = {
        .api_key = "sirio-signal-test-key",
        .provider = SIRIO_PROVIDER_DEEPSEEK,
        .auth_method = SIRIO_AUTH_API_KEY,
    };
    sirio_bridge *bridge = sirio_bridge_create(&bridge_config);
    SIRIO_TEST_ASSERT(bridge != NULL);
    if (bridge != NULL) {
        const char *old_fail_env = getenv("SIRIO_TEST_WORKER_INIT_FAIL");
        char *old_fail = old_fail_env ? xstrdup(old_fail_env) : NULL;
        char stage_text[16];
        snprintf(stage_text, sizeof(stage_text), "%d",
                 SIRIO_TEST_WORKER_INIT_FAIL_CACHE);
        int env_status = setenv("SIRIO_TEST_WORKER_INIT_FAIL",
                                stage_text, 1);
        SIRIO_TEST_ASSERT(env_status == 0);
        if (env_status == 0) {
            sirio_test_expect_signal_handlers = true;
            char *argv[] = {"sirio"};
            sirio_engine engine = {
                .bridge = bridge,
                .provider = SIRIO_PROVIDER_DEEPSEEK,
                .model = sirio_model_find_for_provider(
                    SIRIO_PROVIDER_DEEPSEEK, "deepseek-v4-flash"),
                .reasoning = SIRIO_REASONING_HIGH,
            };
            agent_config config;
            char error[160] = {0};
            SIRIO_TEST_ASSERT(sirio_agent_parse_options(
                &config, 1, argv, error, sizeof(error)) == 0);
            config.gen.raw_prompt = true;
            SIRIO_TEST_ASSERT(sirio_agent_run(&engine, &config) == 1);
            sirio_test_expect_signal_handlers = false;
        }
        if (old_fail != NULL) {
            SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_WORKER_INIT_FAIL",
                                     old_fail, 1) == 0);
            free(old_fail);
        } else {
            SIRIO_TEST_ASSERT(unsetenv("SIRIO_TEST_WORKER_INIT_FAIL") == 0);
        }
        sirio_bridge_destroy(bridge);
    }

    int query_status = sigaction(SIGINT, NULL, &current_action);
    SIRIO_TEST_ASSERT(query_status == 0);
    if (query_status == 0)
        SIRIO_TEST_ASSERT(current_action.sa_handler ==
                          sirio_test_sigint_sentinel);
    query_status = sigaction(SIGPIPE, NULL, &current_action);
    SIRIO_TEST_ASSERT(query_status == 0);
    if (query_status == 0)
        SIRIO_TEST_ASSERT(current_action.sa_handler ==
                          sirio_test_sigint_sentinel);
    SIRIO_TEST_ASSERT(sigaction(SIGPIPE, &old_pipe, NULL) == 0);
    SIRIO_TEST_ASSERT(sigaction(SIGINT, &old_int, NULL) == 0);
}

static void test_worker_lifecycle_failure_cleanup(void) {
    char cache_template[] = "/tmp/sirio-worker-lifecycle-XXXXXX";
    char *cache_root = mkdtemp(cache_template);
    SIRIO_TEST_ASSERT(cache_root != NULL);
    if (cache_root == NULL) return;

    const char *old_home_env = getenv("HOME");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", cache_root, 1) == 0);

    char trace_path[PATH_MAX];
    snprintf(trace_path, sizeof(trace_path), "%s/trace.log", cache_root);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    cfg.gen.raw_prompt = true;
    cfg.gen.trace_path = trace_path;
    struct sirio_engine engine = {
        .bridge = (sirio_bridge *)(uintptr_t)1,
    };

    for (int stage = SIRIO_TEST_WORKER_INIT_FAIL_CACHE;
         stage <= SIRIO_TEST_WORKER_INIT_FAIL_THREAD; stage++) {
        char stage_text[16];
        snprintf(stage_text, sizeof(stage_text), "%d", stage);
        SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_WORKER_INIT_FAIL",
                                 stage_text, 1) == 0);
        SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_CANCEL_POLL_STATE",
                                 "unset", 1) == 0);
        agent_worker w;
        SIRIO_TEST_ASSERT(agent_worker_init(&w, &engine, &cfg) != 0);
        SIRIO_TEST_ASSERT(w.wake_fd[0] == -1);
        SIRIO_TEST_ASSERT(w.wake_fd[1] == -1);
        SIRIO_TEST_ASSERT(w.trace == NULL);
        SIRIO_TEST_ASSERT(w.cache_dir == NULL);
        SIRIO_TEST_ASSERT(sirio_conv.bridge == NULL);
        SIRIO_TEST_ASSERT(!strcmp(getenv("SIRIO_TEST_CANCEL_POLL_STATE"),
                                  "clear"));
    }

    SIRIO_TEST_ASSERT(unsetenv("SIRIO_TEST_WORKER_INIT_FAIL") == 0);
    SIRIO_TEST_ASSERT(setenv("SIRIO_TEST_CANCEL_POLL_STATE",
                             "unset", 1) == 0);
    SIRIO_TEST_ASSERT(chmod(trace_path, 0666) == 0);
    agent_worker w;
    SIRIO_TEST_ASSERT(agent_worker_init(&w, &engine, &cfg) == 0);
    SIRIO_TEST_ASSERT(!strcmp(getenv("SIRIO_TEST_CANCEL_POLL_STATE"),
                              "armed"));
    struct stat trace_stat;
    SIRIO_TEST_ASSERT(stat(trace_path, &trace_stat) == 0);
    SIRIO_TEST_ASSERT((trace_stat.st_mode & 0777) == 0600);
    int wake_read = w.wake_fd[0];
    int wake_write = w.wake_fd[1];
    agent_worker_free(&w);
    SIRIO_TEST_ASSERT(w.wake_fd[0] == -1);
    SIRIO_TEST_ASSERT(w.wake_fd[1] == -1);
    errno = 0;
    SIRIO_TEST_ASSERT(fcntl(wake_read, F_GETFD) == -1 && errno == EBADF);
    errno = 0;
    SIRIO_TEST_ASSERT(fcntl(wake_write, F_GETFD) == -1 && errno == EBADF);
    SIRIO_TEST_ASSERT(!strcmp(getenv("SIRIO_TEST_CANCEL_POLL_STATE"),
                              "clear"));

    char trace_link[PATH_MAX];
    snprintf(trace_link, sizeof(trace_link), "%s/trace-link", cache_root);
    SIRIO_TEST_ASSERT(symlink(trace_path, trace_link) == 0);
    errno = 0;
    FILE *linked_trace = worker_open_trace(trace_link);
    SIRIO_TEST_ASSERT(linked_trace == NULL);
    if (linked_trace) fclose(linked_trace);
    SIRIO_TEST_ASSERT(unlink(trace_link) == 0);

    SIRIO_TEST_ASSERT(unsetenv("SIRIO_TEST_CANCEL_POLL_STATE") == 0);
    if (old_home) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    }

    char sessions_path[PATH_MAX];
    char sirio_path[PATH_MAX];
    snprintf(sessions_path, sizeof(sessions_path), "%s/.sirio/sessions",
             cache_root);
    snprintf(sirio_path, sizeof(sirio_path), "%s/.sirio", cache_root);
    if (unlink(trace_path) != 0)
        SIRIO_TEST_ASSERT(errno == ENOENT);
    SIRIO_TEST_ASSERT(rmdir(sessions_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(sirio_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(cache_root) == 0);
}

static void test_worker_free_releases_owned_fields(void) {
    agent_worker w;
    memset(&w, 0, sizeof(w));
    SIRIO_TEST_ASSERT(pthread_mutex_init(&w.mu, NULL) == 0);
    SIRIO_TEST_ASSERT(pthread_cond_init(&w.cond, NULL) == 0);
    SIRIO_TEST_ASSERT(pipe(w.wake_fd) == 0);
    int wake_read = w.wake_fd[0];
    int wake_write = w.wake_fd[1];
    w.cache_dir = xstrdup("cache");
    w.session_title = xstrdup("title");
    w.queued_user_drain_text = xstrdup("queued");
    w.cmd_text = xstrdup("command");
    w.out = xstrdup("output");
    w.out_len = strlen(w.out);
    w.out_cap = w.out_len + 1;
    w.trace = tmpfile();
    SIRIO_TEST_ASSERT(w.trace != NULL);
    conv_append(1, "conversation");

    agent_worker_free(&w);
    SIRIO_TEST_ASSERT(w.cache_dir == NULL);
    SIRIO_TEST_ASSERT(w.session_title == NULL);
    SIRIO_TEST_ASSERT(w.queued_user_drain_text == NULL);
    SIRIO_TEST_ASSERT(w.cmd_text == NULL);
    SIRIO_TEST_ASSERT(w.out == NULL);
    SIRIO_TEST_ASSERT(w.trace == NULL);
    errno = 0;
    SIRIO_TEST_ASSERT(fcntl(wake_read, F_GETFD) == -1 && errno == EBADF);
    errno = 0;
    SIRIO_TEST_ASSERT(fcntl(wake_write, F_GETFD) == -1 && errno == EBADF);
}

static void test_cloud_session_persistence(void) {
    char dir_template[] = "/tmp/sirio-session-test-XXXXXX";
    char *dir = mkdtemp(dir_template);
    SIRIO_TEST_ASSERT(dir != NULL);
    if (!dir) return;

    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    w.cfg = &cfg;
    struct sirio_engine engine = {
        .provider = SIRIO_PROVIDER_DEEPSEEK,
        .model = sirio_model_find_for_provider(
            SIRIO_PROVIDER_DEEPSEEK, "deepseek-v4-flash"),
        .reasoning = SIRIO_REASONING_HIGH,
        .select = test_select_model,
    };
    w.engine = &engine;
    w.cache_dir = xstrdup(dir);
    w.initialized = true;
    w.user_activity = true;
    w.session_dirty = true;
    conv_clear();
    sirio_conv.provider = engine.provider;
    conv_append(0, "system prompt");
    conv_append(1, "first line\nsecond line");
    sirio_tool_call saved_call = {
        .id = "saved-call",
        .name = "bash",
        .arguments_json = "{\"command\":\"true\"}",
    };
    static const char provider_state[] =
        "[{\"type\":\"reasoning\",\"encrypted_content\":\"opaque\"}]";
    conv_append_assistant_native("assistant answer", "saved reasoning",
                                 provider_state, &saved_call, 1);
    conv_append_tool("saved-call", "all good\n");
    transcript_sync(&w);

    char sha[41] = {0};
    char err[160] = {0};
    int tokens = -1;
    SIRIO_TEST_ASSERT(agent_worker_save_session_now(
        &w, sha, &tokens, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strlen(sha) == 40);
    SIRIO_TEST_ASSERT(tokens ==
                      (int)transcript_estimate_chars(sirio_conv.chars));
    SIRIO_TEST_ASSERT(!w.session_dirty);
    SIRIO_TEST_ASSERT(w.session_title != NULL);
    SIRIO_TEST_ASSERT(!strcmp(w.session_title, "first line second line"));

    char found_sha[41] = {0};
    char *path = NULL;
    SIRIO_TEST_ASSERT(agent_worker_find_session(
        &w, sha, found_sha, &path, err, sizeof(err)));
    SIRIO_TEST_ASSERT(!strcmp(found_sha, sha));
    SIRIO_TEST_ASSERT(path != NULL);

    char alt_sha[41];
    memcpy(alt_sha, sha, sizeof(alt_sha));
    alt_sha[39] = alt_sha[39] == '0' ? '1' : '0';
    char *alt_path = sirio_session_path(dir, alt_sha);
    SIRIO_TEST_ASSERT(link(path, alt_path) == 0);
    char ambiguous[40];
    memcpy(ambiguous, sha, 39);
    ambiguous[39] = '\0';
    err[0] = '\0';
    SIRIO_TEST_ASSERT(!agent_worker_find_session(
        &w, ambiguous, NULL, NULL, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strstr(err, "ambiguous") != NULL);
    unlink(alt_path);
    free(alt_path);

    engine.provider = SIRIO_PROVIDER_OPENAI;
    engine.model = sirio_model_find("gpt-5.6-luna");
    engine.reasoning = SIRIO_REASONING_LOW;
    conv_clear();
    conv_append(0, "replacement system");
    conv_append(1, "replacement user");
    err[0] = '\0';
    test_expect_restoring_session = true;
    SIRIO_TEST_ASSERT(agent_worker_switch_session(
        &w, sha, 1, err, sizeof(err)));
    test_expect_restoring_session = false;
    SIRIO_TEST_ASSERT(!engine.restoring_session);
    SIRIO_TEST_ASSERT(engine.provider == SIRIO_PROVIDER_DEEPSEEK);
    SIRIO_TEST_ASSERT(engine.model &&
                      !strcmp(engine.model->name, "deepseek-v4-flash"));
    SIRIO_TEST_ASSERT(engine.reasoning == SIRIO_REASONING_HIGH);
    SIRIO_TEST_ASSERT(sirio_conv.len == 4);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[0].text, "system prompt"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[1].text,
                              "first line\nsecond line"));
    SIRIO_TEST_ASSERT(sirio_conv.v[3].role == 3);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].reasoning,
                              "saved reasoning"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].provider_state_json,
                              provider_state));
    SIRIO_TEST_ASSERT(sirio_conv.v[2].tool_call_count == 1);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[2].tool_calls[0].arguments_json,
                              "{\"command\":\"true\"}"));
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[3].tool_call_id, "saved-call"));
    SIRIO_TEST_ASSERT(!strcmp(w.session_sha, sha));
    SIRIO_TEST_ASSERT(!w.session_dirty);

    engine.provider = SIRIO_PROVIDER_OPENAI;
    engine.model = sirio_model_find("gpt-5.6-luna");
    engine.reasoning = SIRIO_REASONING_LOW;
    err[0] = '\0';
    SIRIO_TEST_ASSERT(agent_worker_load_session(
        &w, sha, 1, false, err, sizeof(err)));
    SIRIO_TEST_ASSERT(engine.provider == SIRIO_PROVIDER_OPENAI);
    SIRIO_TEST_ASSERT(engine.model &&
                      !strcmp(engine.model->name, "gpt-5.6-luna"));
    SIRIO_TEST_ASSERT(sirio_conv.v[2].provider_state_json == NULL);

    uint32_t stripped_tokens = 0;
    SIRIO_TEST_ASSERT(agent_worker_strip_session(
        &w, sha, found_sha, &stripped_tokens, err, sizeof(err)));
    SIRIO_TEST_ASSERT(stripped_tokens == (uint32_t)tokens);

    agent_worker_list_sessions(&w);
    SIRIO_TEST_ASSERT(w.out != NULL);
    SIRIO_TEST_ASSERT(strstr(w.out, "first line second line") != NULL);

    FILE *corrupt = fopen(path, "wb");
    SIRIO_TEST_ASSERT(corrupt != NULL);
    if (corrupt) {
        fputs(SIRIO_SESSION_MAGIC "\ncreated_at invalid\n", corrupt);
        fclose(corrupt);
    }
    int old_len = sirio_conv.len;
    char *old_first_user = xstrdup(sirio_conv.v[1].text);
    err[0] = '\0';
    SIRIO_TEST_ASSERT(!agent_worker_switch_session(
        &w, sha, 1, err, sizeof(err)));
    SIRIO_TEST_ASSERT(sirio_conv.len == old_len);
    SIRIO_TEST_ASSERT(!strcmp(sirio_conv.v[1].text, old_first_user));
    free(old_first_user);

    err[0] = '\0';
    SIRIO_TEST_ASSERT(!agent_worker_find_session(
        &w, "not-hex", NULL, NULL, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strstr(err, "hex") != NULL);
    SIRIO_TEST_ASSERT(agent_worker_delete_session(
        &w, sha, found_sha, err, sizeof(err)));

    free(path);
    agent_worker_clear_session_identity(&w);
    free(w.cache_dir);
    w.cache_dir = NULL;
    conv_free();
    test_worker_free(&w);
    SIRIO_TEST_ASSERT(rmdir(dir) == 0);
}

static void test_session_validation_uses_call_ids_not_result_order(void) {
    sirio_tool_call calls[] = {
        {
            .id = "call-a",
            .name = "read",
            .arguments_json = "{\"path\":\"a\"}",
        },
        {
            .id = "call-b",
            .name = "read",
            .arguments_json = "{\"path\":\"b\"}",
        },
    };
    sirio_conv_msg messages[] = {
        {
            .role = SIRIO_ROLE_ASSISTANT,
            .text = "",
            .reasoning = "inspect both files",
            .tool_calls = calls,
            .tool_call_count = 2,
        },
        {
            .role = SIRIO_ROLE_TOOL,
            .text = "b contents",
            .tool_call_id = "call-b",
        },
        {
            .role = SIRIO_ROLE_TOOL,
            .text = "a contents",
            .tool_call_id = "call-a",
        },
    };
    char err[160] = {0};
    SIRIO_TEST_ASSERT(sirio_session_validate_messages(
        messages, 3, err, sizeof(err)));

    messages[2].tool_call_id = "call-b";
    err[0] = '\0';
    SIRIO_TEST_ASSERT(!sirio_session_validate_messages(
        messages, 3, err, sizeof(err)));
    SIRIO_TEST_ASSERT(strstr(err, "duplicate tool result") != NULL);
}

static void test_cli_session_actions_are_standalone(void) {
    char root_template[] = "/tmp/sirio-cli-sessions-XXXXXX";
    char *root = mkdtemp(root_template);
    SIRIO_TEST_ASSERT(root != NULL);
    if (!root) return;
    const char *old_home_env = getenv("HOME");
    char *old_home = old_home_env ? xstrdup(old_home_env) : NULL;
    SIRIO_TEST_ASSERT(setenv("HOME", root, 1) == 0);

    char sessions_dir[PATH_MAX];
    snprintf(sessions_dir, sizeof(sessions_dir), "%s/.sirio/sessions", root);
    SIRIO_TEST_ASSERT(agent_mkdir_p(sessions_dir));
    char auth_path[PATH_MAX];
    char defaults_path[PATH_MAX];
    snprintf(auth_path, sizeof(auth_path), "%s/.sirio/auth.json", root);
    snprintf(defaults_path, sizeof(defaults_path),
             "%s/.sirio/default.json", root);
    FILE *fp = fopen(auth_path, "wb");
    SIRIO_TEST_ASSERT(fp != NULL);
    if (fp) { fputs("broken auth", fp); fclose(fp); }
    fp = fopen(defaults_path, "wb");
    SIRIO_TEST_ASSERT(fp != NULL);
    if (fp) { fputs("broken defaults", fp); fclose(fp); }

    sirio_conv_msg messages[2] = {
        {.role = SIRIO_ROLE_SYSTEM, .text = "system"},
        {.role = SIRIO_ROLE_USER, .text = "standalone session"},
    };
    const char *title = "standalone session";
    uint64_t created_at = 123456789;
    uint64_t last_used = 123456999;
    char sha[41];
    agent_session_identity_sha(title, created_at, sha);
    char *path = sirio_session_path(sessions_dir, sha);
    char error[160] = {0};
    SIRIO_TEST_ASSERT(sirio_session_write(
        path, title, created_at, last_used, "deepseek",
        "deepseek-v4-flash", "none", messages, 2,
        error, sizeof(error)));
    struct timespec old_times[2] = {
        {.tv_sec = 123450000, .tv_nsec = 123456789},
        {.tv_sec = 123450001, .tv_nsec = 987654321},
    };
    SIRIO_TEST_ASSERT(utimensat(AT_FDCWD, path, old_times, 0) == 0);

    char *list_argv[] = {"sirio", "sessions", "--list"};
    sirio_main_capture list = sirio_test_capture_main(3, list_argv, NULL);
    SIRIO_TEST_ASSERT(list.status == 0);
    SIRIO_TEST_ASSERT(list.out && strstr(list.out, "standalone session"));
    SIRIO_TEST_ASSERT(!list.err || !strstr(list.err, "auth"));
    sirio_test_capture_free(&list);

    char *strip_argv[] = {"sirio", "sessions", "--strip", sha};
    sirio_main_capture strip = sirio_test_capture_main(4, strip_argv, NULL);
    SIRIO_TEST_ASSERT(strip.status == 0);
    SIRIO_TEST_ASSERT(strip.out && strstr(strip.out, "Rewritten session"));
    sirio_test_capture_free(&strip);
    struct stat after;
    SIRIO_TEST_ASSERT(stat(path, &after) == 0);
    SIRIO_TEST_ASSERT(after.st_mtim.tv_sec == old_times[1].tv_sec);
    SIRIO_TEST_ASSERT(after.st_mtim.tv_nsec == old_times[1].tv_nsec);
    sirio_session_data data;
    SIRIO_TEST_ASSERT(sirio_session_read(
        path, false, &data, error, sizeof(error)));
    SIRIO_TEST_ASSERT(data.last_used == last_used);
    sirio_session_data_free(&data);

    char *delete_argv[] = {
        "sirio", "sessions", "--delete", sha, "--yes",
    };
    sirio_main_capture deleted = sirio_test_capture_main(
        5, delete_argv, NULL);
    SIRIO_TEST_ASSERT(deleted.status == 0);
    SIRIO_TEST_ASSERT(deleted.out && strstr(deleted.out, "Deleted session"));
    SIRIO_TEST_ASSERT(access(path, F_OK) != 0 && errno == ENOENT);
    sirio_test_capture_free(&deleted);

    free(path);
    unlink(auth_path);
    unlink(defaults_path);
    rmdir(sessions_dir);
    char sirio_dir[PATH_MAX];
    snprintf(sirio_dir, sizeof(sirio_dir), "%s/.sirio", root);
    rmdir(sirio_dir);
    if (old_home) {
        SIRIO_TEST_ASSERT(setenv("HOME", old_home, 1) == 0);
        free(old_home);
    } else {
        SIRIO_TEST_ASSERT(unsetenv("HOME") == 0);
    }
    SIRIO_TEST_ASSERT(rmdir(root) == 0);
}

static void test_cloud_help_is_accurate(void) {
    char *text = NULL;
    size_t text_len = 0;
    FILE *fp = open_memstream(&text, &text_len);
    SIRIO_TEST_ASSERT(fp != NULL);
    if (!fp) return;
    sirio_help_print(fp, NULL);
    SIRIO_TEST_ASSERT(fclose(fp) == 0);
    SIRIO_TEST_ASSERT(text_len > 0);
    SIRIO_TEST_ASSERT(strstr(
        text, "Usage: sirio [command] [options]") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "More Info") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--help full") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--help runtime") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--help diagnostics") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Commands") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Options") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Runtime Commands") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Interactive Controls") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Authentication") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Provider And Model") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Agent Options") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Prompt And Sampling") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "  auth") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "  catalog") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "  sessions") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--provider") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--ctx") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Default: provider limit") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--default-provider") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--default-model") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--think-max") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--nothink") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--think") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "ultra") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "/model") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "/provider") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "  --api-key") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "  --resume ID") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "  --delete ID") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Alt+k / Alt+l") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Agent Tool System") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Examples") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Ctrl+C") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "GGUF model path") == NULL);
    SIRIO_TEST_ASSERT(strstr(text, "Set GPU duty cycle") == NULL);
    free(text);

    static const struct {
        const char *topic;
        const char *usage;
        const char *option;
    } command_help[] = {
        {"auth", "Usage: sirio auth [options]", "--api-key"},
        {"catalog", "Usage: sirio catalog [options]", "--providers"},
        {"sessions", "Usage: sirio sessions [options]", "--resume"},
    };
    for (size_t i = 0; i < sizeof(command_help) / sizeof(command_help[0]);
         i++) {
        text = NULL;
        text_len = 0;
        fp = open_memstream(&text, &text_len);
        SIRIO_TEST_ASSERT(fp != NULL);
        if (!fp) return;
        SIRIO_TEST_ASSERT(sirio_help_print(
            fp, command_help[i].topic) == 0);
        SIRIO_TEST_ASSERT(fclose(fp) == 0);
        SIRIO_TEST_ASSERT(strstr(text, command_help[i].usage) != NULL);
        SIRIO_TEST_ASSERT(strstr(text, command_help[i].option) != NULL);
        SIRIO_TEST_ASSERT(strstr(text, "Runtime Commands") == NULL);
        free(text);
    }

    text = NULL;
    text_len = 0;
    fp = open_memstream(&text, &text_len);
    SIRIO_TEST_ASSERT(fp != NULL);
    if (!fp) return;
    SIRIO_TEST_ASSERT(sirio_help_print(
        fp, "runtime") == 2);
    SIRIO_TEST_ASSERT(fclose(fp) == 0);
    SIRIO_TEST_ASSERT(strstr(text,
                             "sirio: unknown help topic 'runtime'") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "--help full") == NULL);
    free(text);

    text = NULL;
    text_len = 0;
    fp = open_memstream(&text, &text_len);
    SIRIO_TEST_ASSERT(fp != NULL);
    if (!fp) return;
    SIRIO_TEST_ASSERT(sirio_help_print(
        fp, "full") == 2);
    SIRIO_TEST_ASSERT(fclose(fp) == 0);
    SIRIO_TEST_ASSERT(strstr(text,
                             "sirio: unknown help topic 'full'") != NULL);
    SIRIO_TEST_ASSERT(strstr(text, "More Info") == NULL);
    free(text);
}

static void test_quit_exit_command_recognition(void) {
    SIRIO_TEST_ASSERT(agent_slash_command_known("/quit"));
    SIRIO_TEST_ASSERT(agent_slash_command_known("/exit"));
    SIRIO_TEST_ASSERT(!agent_slash_command_known("/quit now"));
    SIRIO_TEST_ASSERT(!agent_slash_command_known("/exit now"));
    SIRIO_TEST_ASSERT(agent_slash_command_known("/model sol max"));
    SIRIO_TEST_ASSERT(!agent_slash_command_known("/provider openai"));
    SIRIO_TEST_ASSERT(!agent_slash_command_known("/QUIT"));
    SIRIO_TEST_ASSERT(!agent_slash_command_known("/EXIT"));
}

static agent_exit_save_result test_exit_save_with_input(
        agent_worker *worker, const char *input) {
    int saved_stdin = dup(STDIN_FILENO);
    int input_pipe[2] = {-1, -1};
    bool stdin_redirected = false;
    agent_exit_save_result result = AGENT_EXIT_CANCEL;

    SIRIO_TEST_ASSERT(saved_stdin >= 0);
    if (saved_stdin < 0) goto cleanup;
    int pipe_status = pipe(input_pipe);
    SIRIO_TEST_ASSERT(pipe_status == 0);
    if (pipe_status != 0) goto cleanup;
    size_t input_length = strlen(input);
    ssize_t written = write(input_pipe[1], input, input_length);
    SIRIO_TEST_ASSERT(written == (ssize_t)input_length);
    if (written != (ssize_t)input_length) goto cleanup;
    close(input_pipe[1]);
    input_pipe[1] = -1;
    int redirect_status = dup2(input_pipe[0], STDIN_FILENO);
    SIRIO_TEST_ASSERT(redirect_status == STDIN_FILENO);
    if (redirect_status != STDIN_FILENO) goto cleanup;
    stdin_redirected = true;
    close(input_pipe[0]);
    input_pipe[0] = -1;
    clearerr(stdin);

    result = agent_maybe_save_before_exiting(worker);

cleanup:
    if (stdin_redirected) {
        clearerr(stdin);
        SIRIO_TEST_ASSERT(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    }
    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (input_pipe[1] >= 0) close(input_pipe[1]);
    if (saved_stdin >= 0) close(saved_stdin);
    clearerr(stdin);
    return result;
}

static void test_exit_save_decision_paths(void) {
    char dir_template[] = "/tmp/sirio-exit-save-XXXXXX";
    char *dir = mkdtemp(dir_template);
    SIRIO_TEST_ASSERT(dir != NULL);
    if (dir == NULL) return;

    agent_worker w;
    test_worker_init(&w);
    agent_config cfg;
    sirio_config_defaults(&cfg);
    w.cfg = &cfg;
    struct sirio_engine engine = {
        .provider = SIRIO_PROVIDER_OPENAI,
        .model = sirio_model_find("gpt-5.6-luna"),
        .reasoning = SIRIO_REASONING_LOW,
    };
    w.engine = &engine;
    w.cache_dir = xstrdup(dir);
    w.initialized = true;
    w.status.state = AGENT_WORKER_IDLE;
    conv_clear();
    sirio_conv.provider = engine.provider;
    conv_append(0, "system prompt");
    conv_append(1, "user content");
    transcript_sync(&w);

    SIRIO_TEST_ASSERT(agent_maybe_save_before_exiting(&w) ==
                      AGENT_EXIT_CLEAN);

    w.user_activity = true;
    w.session_dirty = true;
    SIRIO_TEST_ASSERT(test_exit_save_with_input(&w, "n\n") ==
                      AGENT_EXIT_NOW);
    SIRIO_TEST_ASSERT(w.session_dirty);

    SIRIO_TEST_ASSERT(test_exit_save_with_input(&w, "y\n") ==
                      AGENT_EXIT_CLEAN);
    SIRIO_TEST_ASSERT(!w.session_dirty);
    SIRIO_TEST_ASSERT(strlen(w.session_sha) == 40);
    char *saved_path = sirio_session_path(dir, w.session_sha);
    SIRIO_TEST_ASSERT(unlink(saved_path) == 0);
    free(saved_path);

    char blocker_path[PATH_MAX];
    snprintf(blocker_path, sizeof(blocker_path), "%s/not-a-directory", dir);
    FILE *blocker = fopen(blocker_path, "wb");
    SIRIO_TEST_ASSERT(blocker != NULL);
    if (blocker != NULL) SIRIO_TEST_ASSERT(fclose(blocker) == 0);
    free(w.cache_dir);
    w.cache_dir = xstrdup(blocker_path);
    w.session_dirty = true;

    SIRIO_TEST_ASSERT(test_exit_save_with_input(&w, "y\nn\n") ==
                      AGENT_EXIT_CANCEL);
    SIRIO_TEST_ASSERT(w.session_dirty);
    SIRIO_TEST_ASSERT(test_exit_save_with_input(&w, "y\ny\n") ==
                      AGENT_EXIT_NOW);
    SIRIO_TEST_ASSERT(w.session_dirty);

    agent_worker_clear_session_identity(&w);
    free(w.cache_dir);
    w.cache_dir = NULL;
    conv_free();
    test_worker_free(&w);
    SIRIO_TEST_ASSERT(unlink(blocker_path) == 0);
    SIRIO_TEST_ASSERT(rmdir(dir) == 0);
}

static int sirio_test_subprocess_child(int argc, char **argv) {
    const char *depth = getenv(SIRIO_SUBPROCESS_DEPTH_ENV);
    bool specialist = argc == 8 && !strcmp(argv[7], "specialist");
    const char *expected_model = specialist ? "openai/gpt-5.6-sol" :
                                               "opencode-go/deepseek-v4-flash";
    const char *expected_reasoning = specialist ? "high" : "none";
    if (argc != 8 || strcmp(argv[1], "--model") ||
        strcmp(argv[2], expected_model) ||
        strcmp(argv[3], "--think") ||
        strcmp(argv[4], expected_reasoning) ||
        strcmp(argv[5], "--non-interactive") ||
        strcmp(argv[6], "-p") || !depth || strcmp(depth, "1")) {
        fputs("unexpected subagent arguments\n", stderr);
        return 9;
    }
    if (specialist) {
        fputs("specialist answer\n", stdout);
        return 0;
    }
    if (!strcmp(argv[7], "fail")) {
        fputs("partial answer\n", stdout);
        fputs("child failure\n", stderr);
        return 7;
    }
    if (strcmp(argv[7], "delegated task")) {
        fputs("unexpected subagent prompt\n", stderr);
        return 9;
    }
    fputs("delegated answer\n", stdout);
    fputs("child diagnostic\n", stderr);
    return 0;
}

int main(int argc, char **argv) {
    if (getenv("SIRIO_TEST_SUBPROCESS_CHILD"))
        return sirio_test_subprocess_child(argc, argv);
    char *resolved_program_path = realpath(argv[0], NULL);
    sirio_test_program_path = resolved_program_path ?
                              resolved_program_path : argv[0];
    test_compact_keeps_system_and_tail();
    test_compact_failure_preserves_conversation();
    test_compaction_keeps_a_native_exchange_atomic();
    test_long_provider_error_copy_is_bounded();
    test_history_honors_user_turns();
    test_history_formats_assistant_markdown();
    test_cloud_option_mapping();
    test_cloud_option_rejections();
    test_system_prompt_order();
    test_system_prompt_includes_runtime_model_catalog();
    test_reasoning_is_rendered_and_captured();
    test_raw_bridge_events_bypass_stream_parser();
    test_cloud_context_injections();
    test_native_argument_contract_validation();
    test_every_native_tool_has_a_working_portable_schema();
    test_subagent_tool_runs_on_the_host();
    test_thinking_tool_calls_allow_direct_calls();
    test_provider_usage_anchors_context_accounting();
    test_native_multi_tool_round_preserves_provider_order();
    test_invalid_native_arguments_recover_on_next_request();
    test_noninteractive_one_shot_frontend();
    test_noninteractive_stdin_queue_protocol();
    test_cloud_status_omits_local_engine_metrics();
    test_footer_clips_selection_on_the_right();
    test_runtime_selection_and_reasoning_steps();
    test_worker_cancel_poll_tracks_interrupt_latch();
    test_main_parses_options_before_auth();
    test_main_manages_interface_defaults();
    test_main_auth_and_selection_actions();
    test_main_resume_accepts_supported_session_model();
    test_agent_entry_installs_and_restores_signals();
    test_worker_lifecycle_failure_cleanup();
    test_worker_free_releases_owned_fields();
    test_cloud_session_persistence();
    test_session_validation_uses_call_ids_not_result_order();
    test_cli_session_actions_are_standalone();
    test_cloud_help_is_accurate();
    test_quit_exit_command_recognition();
    test_exit_save_decision_paths();
    int failures = sirio_test_failures;
    free(resolved_program_path);
    if (failures) {
        fprintf(stderr, "sirio agent tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("sirio agent tests: ok");
    return 0;
}
