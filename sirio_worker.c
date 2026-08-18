/*
 * sirio_worker.c - autonomous adapted worker layer.
 *
 * This file is compiled independently into sirio_worker.o.  Its public API is
 * declared in sirio_worker.h; the shared integration declarations in
 * sirio_core.h make the retained core/worker dependency explicit.
 *
 * Adaptation notes:
 *
 * - A Sirio "engine" is a provider bridge. Generation uses native function
 *   tools executed by the container runner.
 * - Provider prompt usage anchors context accounting; messages appended after
 *   a request use a conservative character estimate until the next request.
 * - The conversation lives in a small local message list (sirio_conv).
 *   Assistant reasoning/calls, provider-owned replay state, and one tool result
 *   per tool_call_id are kept verbatim so provider continuations stay valid.
 * - Session persistence stores the normalized provider transcript.
 */

#include "sirio_worker.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void worker_status_selection_locked(agent_worker *worker);
static bool agent_worker_load_session(agent_worker *worker,
                                      const char *prefix,
                                      int history_turns,
                                      bool restore_selection,
                                      char *error, size_t error_len);

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char **environ;

#define SIRIO_SUBPROCESS_MAX_DEPTH 4
#define SIRIO_SUBPROCESS_OUTPUT_LIMIT (128u * 1024u)

/* The provider owns call framing; Sirio validates arguments before forwarding. */
static const sirio_tool sirio_native_tools[] = {
    {
        .name = "read",
        .description = "Read a text file or a range of lines.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"start_line\":{\"type\":\"integer\"},\"max_lines\":{\"type\":\"integer\"},\"whole\":{\"type\":\"boolean\"},\"raw\":{\"type\":\"boolean\"}},\"required\":[\"path\"],\"additionalProperties\":false}",
    },
    {
        .name = "more",
        .description = "Continue the previous read-like output.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}},\"additionalProperties\":false}",
    },
    {
        .name = "write",
        .description = "Create or overwrite a text file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],\"additionalProperties\":false}",
    },
    {
        .name = "edit",
        .description = "Replace exactly one old text match.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}},\"required\":[\"path\",\"old\",\"new\"],\"additionalProperties\":false}",
    },
    {
        .name = "list",
        .description = "List one directory compactly.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}",
    },
    {
        .name = "search",
        .description = "Search files and return compact edit-friendly matches.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},\"context\":{\"type\":\"integer\"},\"max_results\":{\"type\":\"integer\"},\"case_sensitive\":{\"type\":\"boolean\"}},\"required\":[\"query\"],\"additionalProperties\":false}",
    },
    {
        .name = "bash",
        .description = "Run a shell command.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout_sec\":{\"type\":\"integer\"},\"refresh_sec\":{\"type\":\"integer\"}},\"required\":[\"command\"],\"additionalProperties\":false}",
    },
    {
        .name = "bash_status",
        .description = "Report current status and new output for a bash job.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"job\":{\"type\":\"integer\"},\"pid\":{\"type\":\"integer\"},\"refresh_sec\":{\"type\":\"integer\"}},\"required\":[\"job\"],\"additionalProperties\":false}",
    },
    {
        .name = "bash_stop",
        .description = "Terminate a running bash job and report its final output.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"job\":{\"type\":\"integer\"},\"pid\":{\"type\":\"integer\"},\"refresh_sec\":{\"type\":\"integer\"}},\"required\":[\"job\"],\"additionalProperties\":false}",
    },
    {
        .name = "google_search",
        .description = "Search Google in a visible browser and return compact Markdown links.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"],\"additionalProperties\":false}",
    },
    {
        .name = "visit_page",
        .description = "Open a URL in a visible browser and return rendered page Markdown.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"],\"additionalProperties\":false}",
    },
    {
        .name = "subprocess",
        .description = "Run a focused task in a separate host-side agent process in the same workspace; model may select any active catalog model and defaults to the current model.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"model\":{\"type\":\"string\"},\"reasoning\":{\"type\":\"string\"}},\"required\":[\"prompt\"],\"additionalProperties\":false}",
    },
};

#define SIRIO_NATIVE_TOOL_COUNT \
    (sizeof(sirio_native_tools) / sizeof(sirio_native_tools[0]))

static const char sirio_native_prompt_intro[] =
    "You are a coding agent running in a local workspace. Use the available "
    "tools for local file, system, and web work. Inspect the real workspace "
    "before making claims, make requested changes with tools, verify them, and "
    "summarize the result briefly. Avoid dumping large files or code blocks in "
    "chat when a tool can inspect or modify them directly.\n\n"
    "Read defaults to a context-sized bounded chunk. For a first look at a "
    "large file, use max_lines around 80-160. When the result reports more "
    "lines, use more with count, or use continue_offset as the next start_line. "
    "Use whole=true only when the complete file is genuinely needed; if it does "
    "not fit, work in chunks. Use raw=true only when line decorations would "
    "corrupt the payload.\n\n";

static const char sirio_native_prompt_edit_exact[] =
    "When editing, identify the target path first. Prefer edit for focused "
    "changes: old must match exactly once in the current file, so read enough "
    "context to supply an exact unique match. To insert text, replace a unique "
    "anchor with that anchor plus the insertion. Use write for new files or "
    "deliberate whole-file replacement.\n\n";

static const char sirio_native_prompt_edit_upto[] =
    "When editing, identify the target path first. Prefer edit for focused "
    "changes: old must match exactly once in the current file. For a large "
    "replacement, old may contain one [upto] marker between a unique multi-line "
    "head and unique final lines. Never end old immediately after [upto], and "
    "do not use a generic closing brace as the tail. Use write for new files or "
    "deliberate whole-file replacement.\n\n";

static const char sirio_native_prompt_tail[] =
    "For long-running bash commands, pass refresh_sec. If a job is still "
    "running, use bash_status to inspect new output or bash_stop to terminate "
    "it. Use subprocess to delegate a focused, self-contained task to another "
    "agent process; it runs on the host in the same workspace and starts its "
    "own tool container. Models outside the configured base interface are available "
    "only through subprocess; use provider/model when selecting one. Use "
    "google_search to discover pages and visit_page to read a "
    "known URL; the first web action may require permission to start a visible "
    "browser. Treat tool errors as actionable observations and retry with "
    "corrected or smaller inputs. Preserve the current system configuration "
    "unless the user explicitly asks to change it.\n";

static char *sirio_build_native_tools_prompt(bool edit_upto) {
    agent_buf prompt = {0};
    agent_buf_puts(&prompt, sirio_native_prompt_intro);
    agent_buf_puts(&prompt, edit_upto ? sirio_native_prompt_edit_upto :
                                      sirio_native_prompt_edit_exact);
    agent_buf_puts(&prompt, sirio_native_prompt_tail);
    return agent_buf_take(&prompt);
}

static char *sirio_build_native_prompt_reminder(bool edit_upto) {
    agent_buf reminder = {0};
    char *prompt = sirio_build_native_tools_prompt(edit_upto);
    agent_buf_puts(&reminder, "\n\n[System prompt reminder follows.]\n");
    agent_buf_puts(&reminder, prompt);
    agent_buf_puts(&reminder, "[End system prompt reminder.]\n\n");
    free(prompt);
    return agent_buf_take(&reminder);
}

/* Conversation state owned by the worker thread. Native assistant calls,
 * reasoning, and opaque provider state remain durable whenever a transport
 * requires exact replay on later requests. */
typedef struct {
    sirio_role role;
    sirio_provider provider;
    char *text;
    char *reasoning;
    char *provider_state_json;
    char *tool_call_id;
    sirio_tool_call *tool_calls;
    size_t tool_call_count;
} sirio_conv_msg;

static struct {
    sirio_conv_msg *v;
    int len;
    int cap;
    int chars;              /* total durable conversation characters */
    int provider_anchor_chars;
    uint64_t provider_prompt_tokens;
    bool provider_usage_valid;
    sirio_provider provider;
    sirio_bridge *bridge;   /* set by agent_worker_init() */
} sirio_conv;

static void conv_tool_call_free(sirio_tool_call *call) {
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments_json);
    memset(call, 0, sizeof(*call));
}

static void conv_msg_free(sirio_conv_msg *message) {
    if (!message) return;
    free(message->text);
    free(message->reasoning);
    free(message->provider_state_json);
    free(message->tool_call_id);
    for (size_t i = 0; i < message->tool_call_count; i++)
        conv_tool_call_free(&message->tool_calls[i]);
    free(message->tool_calls);
    memset(message, 0, sizeof(*message));
}

static size_t sirio_size_add_clamp(size_t left, size_t right) {
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

static size_t conv_msg_chars(const sirio_conv_msg *message) {
    size_t chars = message && message->text ? strlen(message->text) : 0;
    if (!message) return chars;
    if (message->reasoning)
        chars = sirio_size_add_clamp(chars, strlen(message->reasoning));
    if (message->provider_state_json)
        chars = sirio_size_add_clamp(chars,
                                     strlen(message->provider_state_json));
    if (message->tool_call_id)
        chars = sirio_size_add_clamp(chars, strlen(message->tool_call_id));
    for (size_t i = 0; i < message->tool_call_count; i++) {
        const sirio_tool_call *call = &message->tool_calls[i];
        if (call->id)
            chars = sirio_size_add_clamp(chars, strlen(call->id));
        if (call->name)
            chars = sirio_size_add_clamp(chars, strlen(call->name));
        if (call->arguments_json)
            chars = sirio_size_add_clamp(chars,
                                         strlen(call->arguments_json));
    }
    return chars;
}

static int conv_chars_clamp(size_t chars) {
    return chars > (size_t)INT_MAX ? INT_MAX : (int)chars;
}

static void conv_recount_chars(void) {
    size_t chars = 0;
    for (int i = 0; i < sirio_conv.len; i++) {
        size_t add = conv_msg_chars(&sirio_conv.v[i]);
        if (add > (size_t)INT_MAX - chars) {
            chars = INT_MAX;
            break;
        }
        chars += add;
    }
    sirio_conv.chars = conv_chars_clamp(chars);
}

static sirio_conv_msg *conv_push(sirio_role role, const char *text) {
    if (sirio_conv.len == sirio_conv.cap) {
        sirio_conv.cap = sirio_conv.cap ? sirio_conv.cap * 2 : 16;
        sirio_conv.v = xrealloc(sirio_conv.v,
                                (size_t)sirio_conv.cap * sizeof(*sirio_conv.v));
    }
    sirio_conv_msg *message = &sirio_conv.v[sirio_conv.len++];
    memset(message, 0, sizeof(*message));
    message->role = role;
    message->text = xstrdup(text ? text : "");
    return message;
}

static void conv_free(void) {
    for (int i = 0; i < sirio_conv.len; i++) conv_msg_free(&sirio_conv.v[i]);
    free(sirio_conv.v);
    memset(&sirio_conv, 0, sizeof(sirio_conv));
}

static void conv_append(int role, const char *text) {
    sirio_conv_msg *message = conv_push((sirio_role)role, text);
    if ((sirio_role)role == SIRIO_ROLE_ASSISTANT)
        message->provider = sirio_conv.provider;
    size_t add = conv_msg_chars(message);
    if (add > (size_t)INT_MAX - (size_t)sirio_conv.chars)
        sirio_conv.chars = INT_MAX;
    else
        sirio_conv.chars += (int)add;
}

static void conv_append_assistant_native(const char *text,
                                         const char *reasoning,
                                         const char *provider_state_json,
                                         const sirio_tool_call *calls,
                                         size_t call_count) {
    sirio_conv_msg *message = conv_push(SIRIO_ROLE_ASSISTANT, text);
    message->provider = sirio_conv.provider;
    if (reasoning)
        message->reasoning = xstrdup(reasoning);
    if (provider_state_json)
        message->provider_state_json = xstrdup(provider_state_json);
    if (call_count) {
        message->tool_calls = xmalloc(call_count * sizeof(*message->tool_calls));
        memset(message->tool_calls, 0,
               call_count * sizeof(*message->tool_calls));
        message->tool_call_count = call_count;
        for (size_t i = 0; i < call_count; i++) {
            message->tool_calls[i].id = xstrdup(calls[i].id);
            message->tool_calls[i].name = xstrdup(calls[i].name);
            message->tool_calls[i].arguments_json =
                xstrdup(calls[i].arguments_json);
        }
    }
    conv_recount_chars();
}

static void conv_append_tool(const char *tool_call_id, const char *text) {
    sirio_conv_msg *message = conv_push(SIRIO_ROLE_TOOL, text);
    message->tool_call_id = xstrdup(tool_call_id ? tool_call_id : "");
    conv_recount_chars();
}

static void conv_msg_clone(sirio_conv_msg *destination,
                           const sirio_conv_msg *source) {
    memset(destination, 0, sizeof(*destination));
    destination->role = source->role;
    destination->provider = source->provider;
    destination->text = xstrdup(source->text ? source->text : "");
    if (source->reasoning)
        destination->reasoning = xstrdup(source->reasoning);
    if (source->provider_state_json)
        destination->provider_state_json =
            xstrdup(source->provider_state_json);
    if (source->tool_call_id)
        destination->tool_call_id = xstrdup(source->tool_call_id);
    if (source->tool_call_count) {
        destination->tool_calls = xmalloc(
            source->tool_call_count * sizeof(*destination->tool_calls));
        memset(destination->tool_calls, 0,
               source->tool_call_count * sizeof(*destination->tool_calls));
        destination->tool_call_count = source->tool_call_count;
        for (size_t i = 0; i < source->tool_call_count; i++) {
            destination->tool_calls[i].id = xstrdup(source->tool_calls[i].id);
            destination->tool_calls[i].name = xstrdup(source->tool_calls[i].name);
            destination->tool_calls[i].arguments_json =
                xstrdup(source->tool_calls[i].arguments_json);
        }
    }
}

static void conv_clear(void) {
    for (int i = 0; i < sirio_conv.len; i++) conv_msg_free(&sirio_conv.v[i]);
    sirio_conv.len = 0;
    sirio_conv.chars = 0;
    sirio_conv.provider_anchor_chars = 0;
    sirio_conv.provider_prompt_tokens = 0;
    sirio_conv.provider_usage_valid = false;
}

static uint64_t transcript_estimate_chars(int chars) {
    if (chars <= 0) return 0;
    return ((uint64_t)chars + 3) / 4;
}

/* Keep the retained upstream context/status fields synchronized. The latest
 * provider prompt count is authoritative for the exact request transcript;
 * only messages appended locally after that request are estimated. */
static void transcript_sync(agent_worker *w) {
    uint64_t tokens = transcript_estimate_chars(sirio_conv.chars);
    if (sirio_conv.provider_usage_valid &&
        sirio_conv.chars >= sirio_conv.provider_anchor_chars) {
        tokens = sirio_conv.provider_prompt_tokens +
                 transcript_estimate_chars(
                     sirio_conv.chars - sirio_conv.provider_anchor_chars);
    }
    w->context_used = tokens > (uint64_t)INT_MAX ? INT_MAX : (int)tokens;
    w->status.ctx_used = w->context_used;
}

/* agent_buf has no free helper in the kept code; release it here. */
static void agent_buf_release(agent_buf *b) {
    free(b->ptr);
    memset(b, 0, sizeof(*b));
}

/* Sirio-specific identity text appended after the native-tool behavior prompt. */
static const char sirio_default_system_extra[] =
    "You are Sirio, a concise coding agent running in a local workspace. "
    "Inspect and modify files with the available tools, then summarize "
    "results briefly.";

/* Build a borrowed bridge view over the durable conversation. */
static sirio_message *conv_build_messages(int *count_out) {
    sirio_message *msgs = calloc(sirio_conv.len ? (size_t)sirio_conv.len : 1,
                                 sizeof(*msgs));
    int n = 0;
    for (int i = 0; i < sirio_conv.len; i++) {
        sirio_conv_msg *m = &sirio_conv.v[i];
        msgs[n].role = m->role;
        msgs[n].provider = m->provider;
        msgs[n].content = m->text;
        msgs[n].reasoning = m->reasoning;
        msgs[n].provider_state_json = m->provider_state_json;
        msgs[n].tool_call_id = m->tool_call_id;
        msgs[n].tool_calls = m->tool_calls;
        msgs[n].tool_call_count = m->tool_call_count;
        n++;
    }
    *count_out = n;
    return msgs;
}

static void conv_messages_free(sirio_message *msgs, int n) {
    (void)n;
    free(msgs); /* content pointers are borrowed from sirio_conv */
}

/* ------------------------------------------------------------------ */
/* Generation context and bridge event callback.                        */
/* ------------------------------------------------------------------ */

typedef struct {
    agent_worker *w;
    agent_token_renderer *renderer;
    agent_buf *gen_text;
    agent_buf reasoning_text;
    agent_buf provider_state_json;
    sirio_tool_call *tool_calls;
    size_t tool_call_count;
    size_t tool_call_capacity;
    bool enable_tools;
    bool raw_output;
    bool reasoning_open;
    bool saw_reasoning;
    bool has_usage;
    bool saw_done;
    int request_chars;
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    uint64_t total_tokens;
    uint64_t reasoning_tokens;
    uint64_t prompt_cache_hit_tokens;
    uint64_t prompt_cache_miss_tokens;
    char request_id[128];
    char model[128];
    char finish_reason[64];
    char err[256];
    int failed;
} worker_gen_ctx;

static void worker_copy_err(char *dst, size_t dst_len, const char *src);

static void worker_close_reasoning(worker_gen_ctx *g) {
    if (g->raw_output || !g->reasoning_open) return;
    renderer_process(g->renderer, "</think>\n", strlen("</think>\n"), false);
    g->reasoning_open = false;
}

static void worker_gen_ctx_free(worker_gen_ctx *g) {
    if (!g) return;
    for (size_t i = 0; i < g->tool_call_count; i++)
        conv_tool_call_free(&g->tool_calls[i]);
    free(g->tool_calls);
    g->tool_calls = NULL;
    g->tool_call_count = 0;
    g->tool_call_capacity = 0;
    agent_buf_release(&g->reasoning_text);
    agent_buf_release(&g->provider_state_json);
}

static int worker_gen_add_tool_call(worker_gen_ctx *g,
                                    const sirio_bridge_event *event) {
    if (!event->tool_call_id || !event->tool_call_id[0] ||
        !event->tool_name || !event->tool_name[0] ||
        !event->tool_arguments_json) {
        worker_copy_err(g->err, sizeof(g->err),
                        "provider delivered an incomplete tool call");
        g->failed = 1;
        return 1;
    }
    for (size_t i = 0; i < g->tool_call_count; i++) {
        if (!strcmp(g->tool_calls[i].id, event->tool_call_id)) {
            worker_copy_err(g->err, sizeof(g->err),
                            "provider delivered a duplicate tool-call id");
            g->failed = 1;
            return 1;
        }
    }
    if (g->tool_call_count == g->tool_call_capacity) {
        size_t capacity = g->tool_call_capacity ?
                          g->tool_call_capacity * 2 : 4;
        if (capacity > 1024) {
            worker_copy_err(g->err, sizeof(g->err),
                            "provider delivered too many tool calls");
            g->failed = 1;
            return 1;
        }
        g->tool_calls = xrealloc(g->tool_calls,
                                 capacity * sizeof(*g->tool_calls));
        memset(g->tool_calls + g->tool_call_capacity, 0,
               (capacity - g->tool_call_capacity) * sizeof(*g->tool_calls));
        g->tool_call_capacity = capacity;
    }
    sirio_tool_call *call = &g->tool_calls[g->tool_call_count++];
    call->id = xstrdup(event->tool_call_id);
    call->name = xstrdup(event->tool_name);
    call->arguments_json = xstrdup(event->tool_arguments_json);
    return 0;
}

static int worker_gen_event(const sirio_bridge_event *event, void *private_data) {
    worker_gen_ctx *g = private_data;
    if (!event) return 0;

    if (event->type == SIRIO_BRIDGE_EVENT_USAGE) {
        g->prompt_tokens = event->prompt_tokens;
        g->completion_tokens = event->completion_tokens;
        g->total_tokens = event->total_tokens;
        g->reasoning_tokens = event->reasoning_tokens;
        g->prompt_cache_hit_tokens = event->prompt_cache_hit_tokens;
        g->prompt_cache_miss_tokens = event->prompt_cache_miss_tokens;
        g->has_usage = true;
        return 0;
    }
    if (event->type == SIRIO_BRIDGE_EVENT_DONE) {
        worker_close_reasoning(g);
        if (event->request_id)
            worker_copy_err(g->request_id, sizeof(g->request_id),
                            event->request_id);
        if (event->model)
            worker_copy_err(g->model, sizeof(g->model), event->model);
        if (event->finish_reason)
            worker_copy_err(g->finish_reason, sizeof(g->finish_reason),
                            event->finish_reason);
        g->saw_done = true;
        return 0;
    }

    if (event->type == SIRIO_BRIDGE_EVENT_TOOL_CALL) {
        if (!g->enable_tools) {
            worker_copy_err(g->err, sizeof(g->err),
                            "provider attempted a tool call in tool-free mode");
            g->failed = 1;
            return 1;
        }
        return worker_gen_add_tool_call(g, event);
    }

    if (event->type == SIRIO_BRIDGE_EVENT_PROVIDER_STATE) {
        if (!event->text || !event->text[0] || g->provider_state_json.len) {
            worker_copy_err(g->err, sizeof(g->err),
                            "provider delivered invalid continuation state");
            g->failed = 1;
            return 1;
        }
        agent_buf_puts(&g->provider_state_json, event->text);
        return 0;
    }

    if (g->raw_output) {
        if (event->type == SIRIO_BRIDGE_EVENT_REASONING && event->text) {
            agent_publish(g->w, event->text, strlen(event->text));
        } else if (event->type == SIRIO_BRIDGE_EVENT_TEXT && event->text) {
            agent_buf_puts(g->gen_text, event->text);
            agent_publish(g->w, event->text, strlen(event->text));
        } else if (event->type == SIRIO_BRIDGE_EVENT_ERROR) {
            snprintf(g->err, sizeof(g->err), "%s",
                     event->error ? event->error : "bridge error");
            g->failed = 1;
            return 1;
        }
        return 0;
    }

    if (event->type == SIRIO_BRIDGE_EVENT_REASONING && event->text) {
        g->saw_reasoning = true;
        if (!g->reasoning_open) {
            renderer_process(g->renderer, "<think>", strlen("<think>"), false);
            g->reasoning_open = true;
        }
        agent_buf_puts(&g->reasoning_text, event->text);
        renderer_process(g->renderer, event->text, strlen(event->text), false);
    } else if (event->type == SIRIO_BRIDGE_EVENT_TEXT && event->text) {
        worker_close_reasoning(g);
        const char *text = event->text;
        size_t len = strlen(text);
        agent_buf_puts(g->gen_text, text);
        renderer_process(g->renderer, text, len, false);
    } else if (event->type == SIRIO_BRIDGE_EVENT_ERROR) {
        worker_close_reasoning(g);
        snprintf(g->err, sizeof(g->err), "%s",
                 event->error ? event->error : "bridge error");
        g->failed = 1;
        return 1; /* stop the bridge from emitting more events */
    }
    return 0;
}

/* Bounded error-string copy: err buffers are smaller than g->err, and
 * snprintf("%s") truncation warnings are noise we don't want. */
static void worker_copy_err(char *dst, size_t dst_len, const char *src) {
    if (!dst_len) return;
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Run one bridge generation over the current conversation. */
#ifdef SIRIO_CORE_TEST
typedef int (*worker_generate_fn)(
    sirio_bridge *, const sirio_message *, size_t,
    const sirio_tool *, size_t, sirio_bridge_event_callback, void *);
static worker_generate_fn sirio_test_generate;
#endif

static int worker_generate(agent_worker *w, worker_gen_ctx *g,
                           char *err, size_t err_len) {
    if (!sirio_conv.bridge) {
        snprintf(err, err_len, "bridge is not initialized");
        return 1;
    }
    int n = 0;
    g->request_chars = sirio_conv.chars;
    sirio_message *msgs = conv_build_messages(&n);
    const sirio_tool *tools = g->enable_tools ? sirio_native_tools : NULL;
    size_t tool_count = g->enable_tools ? SIRIO_NATIVE_TOOL_COUNT : 0;
#ifdef SIRIO_CORE_TEST
    int rc = sirio_test_generate ?
        sirio_test_generate(sirio_conv.bridge, msgs, (size_t)n,
                            tools, tool_count, worker_gen_event, g) :
        sirio_bridge_generate(sirio_conv.bridge, msgs, (size_t)n,
                              tools, tool_count, worker_gen_event, g);
#else
    int rc = sirio_bridge_generate(sirio_conv.bridge, msgs, (size_t)n,
                                   tools, tool_count, worker_gen_event, g);
#endif
    conv_messages_free(msgs, n);
    if (rc != 0) {
        if (!g->err[0])
            worker_copy_err(g->err, sizeof(g->err),
                            sirio_bridge_last_error(sirio_conv.bridge));
        worker_copy_err(err, err_len, g->err);
        return 1;
    }
    if (g->failed) {
        worker_copy_err(err, err_len,
                        g->err[0] ? g->err : "generation failed");
        return 1;
    }
    if (!g->saw_done) {
        worker_copy_err(err, err_len,
                        "provider stream ended without completion metadata");
        return 1;
    }
    if (strcmp(g->finish_reason, "stop") &&
        strcmp(g->finish_reason, "tool_calls")) {
        snprintf(g->err, sizeof(g->err),
                 "provider stopped generation with finish reason: %.160s",
                 g->finish_reason[0] ? g->finish_reason : "unknown");
        worker_copy_err(err, err_len, g->err);
        return 1;
    }
    bool tool_finish = !strcmp(g->finish_reason, "tool_calls");
    if (tool_finish && !g->tool_call_count) {
        worker_copy_err(g->err, sizeof(g->err),
                        "provider finished with tool_calls but delivered no calls");
        worker_copy_err(err, err_len, g->err);
        return 1;
    }
    if (!tool_finish && g->tool_call_count) {
        worker_copy_err(g->err, sizeof(g->err),
                        "provider delivered tool calls with a stop finish reason");
        worker_copy_err(err, err_len, g->err);
        return 1;
    }
    if (g->has_usage) {
        sirio_conv.provider_anchor_chars = g->request_chars;
        sirio_conv.provider_prompt_tokens = g->prompt_tokens;
        sirio_conv.provider_usage_valid = true;
        transcript_sync(w);
    }
    agent_trace(w,
                "provider request=%s model=%s finish=%s prompt=%llu completion=%llu total=%llu reasoning=%llu cache_hit=%llu cache_miss=%llu",
                g->request_id[0] ? g->request_id : "unknown",
                g->model[0] ? g->model : "unknown",
                g->finish_reason,
                (unsigned long long)g->prompt_tokens,
                (unsigned long long)g->completion_tokens,
                (unsigned long long)g->total_tokens,
                (unsigned long long)g->reasoning_tokens,
                (unsigned long long)g->prompt_cache_hit_tokens,
                (unsigned long long)g->prompt_cache_miss_tokens);
    return 0;
}

/* Cloud-message equivalents of the upstream token-transcript injections. */
static void agent_worker_maybe_append_datetime_context(agent_worker *w) {
    if (w->datetime_context_injected) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char when[128];
    if (strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %Z", &tm) == 0)
        snprintf(when, sizeof(when), "%lld", (long long)now);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Current local date and time at session start: %s. "
             "Use this only when date or time matters.", when);
    conv_append(0, msg);
    transcript_sync(w);
    agent_trace_text(w, "datetime-context", msg, strlen(msg));
    w->datetime_context_injected = true;
}

static void agent_worker_maybe_append_system_prompt_reminder(agent_worker *w) {
    if (w->last_system_prompt_reminder_at <= 0) {
        agent_worker_note_system_prompt_seen(w);
        return;
    }
    if (w->context_used - w->last_system_prompt_reminder_at <
        AGENT_SYSTEM_PROMPT_REMINDER_TOKENS) {
        return;
    }

    char *reminder = sirio_build_native_prompt_reminder(w->cfg->edit_upto);
    agent_publish_system_status(w, "Re-injecting system prompt reminder...");
    agent_trace(w, "system prompt reminder injected at transcript=%d",
                w->context_used);
    conv_append(0, reminder);
    free(reminder);
    if (w->cfg->gen.system && w->cfg->gen.system[0])
        conv_append(0, w->cfg->gen.system);
    transcript_sync(w);
    agent_worker_note_system_prompt_seen(w);
}


/* ------------------------------------------------------------------ */
/* Adapted worker thread.                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    agent_tool_call call;
    char *result;
    bool arguments_valid;
} worker_native_result;

static void worker_tool_call_free(agent_tool_call *call) {
    if (!call) return;
    free(call->name);
    for (int i = 0; i < call->argc; i++) {
        free(call->args[i].name);
        free(call->args[i].value);
    }
    free(call->args);
    memset(call, 0, sizeof(*call));
}

static void worker_tool_call_add_arg(agent_tool_call *call, const char *name,
                                     const char *value, size_t value_length,
                                     bool is_string) {
    if (call->argc == call->argcap) {
        call->argcap = call->argcap ? call->argcap * 2 : 4;
        call->args = xrealloc(call->args,
                              (size_t)call->argcap * sizeof(call->args[0]));
    }
    call->args[call->argc++] = (agent_tool_arg){
        .name = xstrdup(name),
        .value = xstrndup(value, value_length),
        .is_string = is_string,
    };
}

static const char *worker_tool_arg_value(const agent_tool_call *call,
                                         const char *name) {
    for (int i = 0; i < call->argc; i++)
        if (call->args[i].name && strcmp(call->args[i].name, name) == 0)
            return call->args[i].value ? call->args[i].value : "";
    return NULL;
}

static int worker_tool_result_reserve_tokens(agent_worker *worker) {
    int context = agent_worker_effective_ctx_size(worker);
    int reserve = 1024;
    if (context > 0 && reserve > context / 8)
        reserve = context / 8 > 16 ? context / 8 : 16;
    return reserve;
}

static const sirio_tool *worker_native_tool_find(const char *name) {
    for (size_t i = 0; i < SIRIO_NATIVE_TOOL_COUNT; i++)
        if (!strcmp(name, sirio_native_tools[i].name))
            return &sirio_native_tools[i];
    return NULL;
}

static void worker_native_results_free(worker_native_result *results,
                                       size_t count) {
    for (size_t i = 0; i < count; i++) {
        worker_tool_call_free(&results[i].call);
        free(results[i].result);
    }
    free(results);
}

static bool worker_native_call_convert(const sirio_tool_call *native,
                                       agent_tool_call *call,
                                       char *error, size_t error_len) {
    memset(call, 0, sizeof(*call));
    call->name = xstrdup(native->name ? native->name : "");
    const sirio_tool *tool = worker_native_tool_find(call->name);
    if (!tool) {
        if (error && error_len)
            snprintf(error, error_len, "unknown tool name: %.80s",
                     call->name);
        return false;
    }
    sirio_tool_argument *arguments = NULL;
    size_t argument_count = 0;
    if (sirio_tool_arguments_parse_validated(
            native->arguments_json, tool->input_schema_json,
            &arguments, &argument_count, error, error_len) != 0)
        return false;
    for (size_t i = 0; i < argument_count; i++) {
        worker_tool_call_add_arg(call, arguments[i].name,
                                 arguments[i].value,
                                 strlen(arguments[i].value),
                                 arguments[i].type ==
                                     SIRIO_TOOL_ARGUMENT_STRING);
    }
    sirio_tool_arguments_free(arguments, argument_count);
    return true;
}

static const char *worker_native_display_detail(const agent_tool_call *call) {
    const char *name = call->name ? call->name : "";
    if (!strcmp(name, "bash")) return worker_tool_arg_value(call, "command");
    if (!strcmp(name, "subprocess"))
        return worker_tool_arg_value(call, "prompt");
    if (!strcmp(name, "search") || !strcmp(name, "google_search"))
        return worker_tool_arg_value(call, "query");
    if (!strcmp(name, "visit_page")) return worker_tool_arg_value(call, "url");
    if (!strcmp(name, "bash_status") || !strcmp(name, "bash_stop"))
        return worker_tool_arg_value(call, "job");
    if (!strcmp(name, "more")) return worker_tool_arg_value(call, "count");
    return worker_tool_arg_value(call, "path");
}

static void worker_native_display_call(agent_worker *w,
                                       const agent_tool_call *call,
                                       bool arguments_valid) {
    char detail[181] = {0};
    const char *source = arguments_valid ?
                         worker_native_display_detail(call) : NULL;
    if (source) {
        size_t n = strlen(source);
        if (n >= sizeof(detail)) n = sizeof(detail) - 1;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)source[i];
            detail[i] = c == '\n' || c == '\r' || c == '\t' ? ' ' :
                        (char)c;
        }
        detail[n] = '\0';
    }
    const char *display_name = call->name &&
        worker_native_tool_find(call->name) ? call->name : "unknown-tool";
    agent_publishf(w, "🛠️ %s%s%s%s\n",
                   display_name,
                   detail[0] ? " " : "",
                   detail,
                   arguments_valid ? "" : " [invalid arguments]");
}

static bool worker_subprocess_depth(unsigned *depth_out,
                                    char *error, size_t error_len) {
    const char *text = getenv(SIRIO_SUBPROCESS_DEPTH_ENV);
    if (!text || !text[0]) {
        *depth_out = 0;
        return true;
    }
    for (const char *cursor = text; *cursor; cursor++) {
        if (isdigit((unsigned char)*cursor)) continue;
        snprintf(error, error_len, "%s must be a nonnegative integer",
                 SIRIO_SUBPROCESS_DEPTH_ENV);
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long depth = strtoul(text, &end, 10);
    if (errno || !end || *end || depth > UINT_MAX) {
        snprintf(error, error_len, "%s is out of range",
                 SIRIO_SUBPROCESS_DEPTH_ENV);
        return false;
    }
    *depth_out = (unsigned)depth;
    return true;
}

static void worker_close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static bool worker_capture_pipe(int descriptors[2],
                                char *error, size_t error_len) {
    descriptors[0] = descriptors[1] = -1;
    if (pipe(descriptors) != 0) {
        snprintf(error, error_len, "pipe: %s", strerror(errno));
        return false;
    }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(descriptors[i], F_GETFD);
        if (flags < 0 ||
            fcntl(descriptors[i], F_SETFD, flags | FD_CLOEXEC) != 0) {
            snprintf(error, error_len, "pipe flags: %s", strerror(errno));
            worker_close_fd(&descriptors[0]);
            worker_close_fd(&descriptors[1]);
            return false;
        }
    }
    int flags = fcntl(descriptors[0], F_GETFL);
    if (flags < 0 ||
        fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        snprintf(error, error_len, "pipe mode: %s", strerror(errno));
        worker_close_fd(&descriptors[0]);
        worker_close_fd(&descriptors[1]);
        return false;
    }
    return true;
}

static char **worker_subprocess_environment(unsigned depth,
                                            char **depth_entry_out) {
    static const char prefix[] = SIRIO_SUBPROCESS_DEPTH_ENV "=";
    size_t count = 0;
    while (environ && environ[count]) count++;

    char value[32];
    snprintf(value, sizeof(value), "%u", depth);
    size_t entry_length = sizeof(prefix) - 1 + strlen(value) + 1;
    char *depth_entry = xmalloc(entry_length);
    snprintf(depth_entry, entry_length, "%s%s", prefix, value);

    char **environment = xmalloc((count + 2) * sizeof(*environment));
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (!strncmp(environ[i], prefix, sizeof(prefix) - 1)) continue;
        environment[used++] = environ[i];
    }
    environment[used++] = depth_entry;
    environment[used] = NULL;
    *depth_entry_out = depth_entry;
    return environment;
}

static int worker_spawn_subprocess(const char *executable,
                                   const char *model,
                                   const char *reasoning,
                                   const char *prompt,
                                   unsigned depth,
                                   pid_t *pid_out,
                                   int *stdout_fd_out,
                                   int *stderr_fd_out,
                                   char *error, size_t error_len) {
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (!worker_capture_pipe(stdout_pipe, error, error_len)) return -1;
    if (!worker_capture_pipe(stderr_pipe, error, error_len)) {
        worker_close_fd(&stdout_pipe[0]);
        worker_close_fd(&stdout_pipe[1]);
        return -1;
    }

    posix_spawn_file_actions_t actions;
    int action_status = posix_spawn_file_actions_init(&actions);
    bool actions_initialized = action_status == 0;
    if (action_status == 0)
        action_status = posix_spawn_file_actions_adddup2(
            &actions, stdout_pipe[1], STDOUT_FILENO);
    if (action_status == 0)
        action_status = posix_spawn_file_actions_adddup2(
            &actions, stderr_pipe[1], STDERR_FILENO);
    if (action_status == 0)
        action_status = posix_spawn_file_actions_addclose(
            &actions, stdout_pipe[0]);
    if (action_status == 0)
        action_status = posix_spawn_file_actions_addclose(
            &actions, stdout_pipe[1]);
    if (action_status == 0)
        action_status = posix_spawn_file_actions_addclose(
            &actions, stderr_pipe[0]);
    if (action_status == 0)
        action_status = posix_spawn_file_actions_addclose(
            &actions, stderr_pipe[1]);

    int spawn_status = action_status;
    pid_t pid = -1;
    if (action_status == 0) {
        char *child_argv[] = {
            (char *)executable,
            "--model", (char *)model,
            "--think", (char *)reasoning,
            "--non-interactive", "-p", (char *)prompt,
            NULL,
        };
        char *depth_entry = NULL;
        char **environment = worker_subprocess_environment(
            depth + 1, &depth_entry);
        spawn_status = strchr(executable, '/') ?
            posix_spawn(&pid, executable, &actions, NULL,
                        child_argv, environment) :
            posix_spawnp(&pid, executable, &actions, NULL,
                         child_argv, environment);
        free(environment);
        free(depth_entry);
    }
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);

    worker_close_fd(&stdout_pipe[1]);
    worker_close_fd(&stderr_pipe[1]);
    if (spawn_status != 0) {
        worker_close_fd(&stdout_pipe[0]);
        worker_close_fd(&stderr_pipe[0]);
        snprintf(error, error_len, "%s", strerror(spawn_status));
        return -1;
    }
    *pid_out = pid;
    *stdout_fd_out = stdout_pipe[0];
    *stderr_fd_out = stderr_pipe[0];
    return 0;
}

static void worker_capture_append(agent_buf *buffer,
                                  char *data, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (data[i] == '\0') data[i] = ' ';
    if (buffer->len >= SIRIO_SUBPROCESS_OUTPUT_LIMIT) {
        buffer->truncated = true;
        return;
    }
    if (length > SIRIO_SUBPROCESS_OUTPUT_LIMIT - buffer->len) {
        length = SIRIO_SUBPROCESS_OUTPUT_LIMIT - buffer->len;
        buffer->truncated = true;
    }
    if (buffer->len + length + 1 > buffer->cap) {
        size_t capacity = buffer->cap ? buffer->cap * 2 : 4096;
        while (capacity < buffer->len + length + 1) capacity *= 2;
        buffer->ptr = xrealloc(buffer->ptr, capacity);
        buffer->cap = capacity;
    }
    memcpy(buffer->ptr + buffer->len, data, length);
    buffer->len += length;
    buffer->ptr[buffer->len] = '\0';
}

static int worker_capture_drain(int *fd, agent_buf *buffer) {
    char data[4096];
    for (;;) {
        ssize_t length = read(*fd, data, sizeof(data));
        if (length > 0) {
            worker_capture_append(buffer, data, (size_t)length);
            continue;
        }
        if (length == 0) {
            worker_close_fd(fd);
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        int read_error = errno;
        worker_close_fd(fd);
        return read_error;
    }
}

static int worker_wait_for_subprocess(agent_worker *worker, pid_t pid,
                                      int stdout_fd, int stderr_fd,
                                      agent_buf *stdout_capture,
                                      agent_buf *stderr_capture,
                                      int *status_out,
                                      char *error, size_t error_len) {
    bool interrupt_sent = false;
    int status = 0;
    for (;;) {
        if (!interrupt_sent && worker_should_interrupt(worker)) {
            if (kill(pid, SIGINT) == 0 || errno == ESRCH)
                interrupt_sent = true;
        }

        struct pollfd descriptors[2] = {
            {.fd = stdout_fd, .events = POLLIN},
            {.fd = stderr_fd, .events = POLLIN},
        };
        int poll_status = poll(descriptors, 2, 100);
        if (poll_status < 0 && errno != EINTR) {
            snprintf(error, error_len, "poll: %s", strerror(errno));
            kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            worker_close_fd(&stdout_fd);
            worker_close_fd(&stderr_fd);
            return -1;
        }
        if (poll_status > 0) {
            if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                int read_error = worker_capture_drain(
                    &stdout_fd, stdout_capture);
                if (read_error) {
                    snprintf(error, error_len, "read stdout: %s",
                             strerror(read_error));
                    kill(pid, SIGTERM);
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                    worker_close_fd(&stderr_fd);
                    return -1;
                }
            }
            if (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                int read_error = worker_capture_drain(
                    &stderr_fd, stderr_capture);
                if (read_error) {
                    snprintf(error, error_len, "read stderr: %s",
                             strerror(read_error));
                    kill(pid, SIGTERM);
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                    worker_close_fd(&stdout_fd);
                    return -1;
                }
            }
        }

        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == 0) continue;
        if (waited < 0) {
            if (errno == EINTR) continue;
            snprintf(error, error_len, "waitpid: %s", strerror(errno));
            worker_close_fd(&stdout_fd);
            worker_close_fd(&stderr_fd);
            return -1;
        }
        (void)worker_capture_drain(&stdout_fd, stdout_capture);
        (void)worker_capture_drain(&stderr_fd, stderr_capture);
        worker_close_fd(&stdout_fd);
        worker_close_fd(&stderr_fd);
        *status_out = status;
        return 0;
    }
}

static char *worker_capture_take(agent_buf *capture) {
    bool truncated = capture->truncated;
    char *output = agent_buf_take(capture);
    if (!truncated) return output;
    static const char marker[] = "\n[output truncated]\n";
    size_t length = strlen(output);
    output = xrealloc(output, length + sizeof(marker));
    memcpy(output + length, marker, sizeof(marker));
    return output;
}

static char *worker_subprocess_tool_error(const char *message) {
    agent_buf output = {0};
    agent_buf_puts(&output, "Tool error: ");
    agent_buf_puts(&output, message);
    agent_buf_puts(&output, "\n");
    return agent_buf_take(&output);
}

static char *worker_execute_subprocess_tool(agent_worker *worker,
                                            const agent_tool_call *call) {
    const char *prompt = worker_tool_arg_value(call, "prompt");
    if (!prompt || !prompt[0])
        return worker_subprocess_tool_error(
            "subprocess requires a non-empty prompt");

    char inherited_model[160] = {0};
    const char *model = worker_tool_arg_value(call, "model");
    if (model && !model[0])
        return worker_subprocess_tool_error(
            "subprocess model cannot be empty");
    if (!model && worker->engine) {
        const char *provider = sirio_provider_name(worker->engine->provider);
        int length = provider && worker->engine->model ?
            snprintf(inherited_model, sizeof(inherited_model), "%s/%s",
                     provider, worker->engine->model->name) : -1;
        if (length > 0 && (size_t)length < sizeof(inherited_model)) {
            model = inherited_model;
        }
    }
    if (!model || !model[0])
        return worker_subprocess_tool_error("current model is unavailable");

    const char *reasoning = worker_tool_arg_value(call, "reasoning");
    if (!reasoning && worker->engine)
        reasoning = sirio_reasoning_name(worker->engine->reasoning);
    if (!reasoning || !sirio_reasoning_parse(reasoning, NULL))
        return worker_subprocess_tool_error(
            "subprocess reasoning must be none, low, medium, high, xhigh, "
            "or max");

    unsigned depth = 0;
    char error[256] = {0};
    if (!worker_subprocess_depth(&depth, error, sizeof(error)))
        return worker_subprocess_tool_error(error);
    if (depth >= SIRIO_SUBPROCESS_MAX_DEPTH) {
        char limit[96];
        snprintf(limit, sizeof(limit),
                 "subprocess depth limit (%d) reached",
                 SIRIO_SUBPROCESS_MAX_DEPTH);
        return worker_subprocess_tool_error(limit);
    }
    if (!worker->cfg->executable_path ||
        !worker->cfg->executable_path[0])
        return worker_subprocess_tool_error(
            "host agent executable path is unavailable");

    pid_t pid;
    int stdout_fd;
    int stderr_fd;
    if (worker_spawn_subprocess(worker->cfg->executable_path,
                           model, reasoning, prompt, depth,
                           &pid, &stdout_fd, &stderr_fd,
                           error, sizeof(error)) != 0) {
        agent_buf message = {0};
        agent_buf_puts(&message, "cannot start subprocess agent: ");
        agent_buf_puts(&message, error[0] ? error : "unknown error");
        char *detail = agent_buf_take(&message);
        char *result = worker_subprocess_tool_error(detail);
        free(detail);
        return result;
    }

    agent_buf stdout_capture = {0};
    agent_buf stderr_capture = {0};
    int status = 0;
    int wait_status = worker_wait_for_subprocess(
        worker, pid, stdout_fd, stderr_fd,
        &stdout_capture, &stderr_capture, &status,
        error, sizeof(error));
    char *stdout_text = worker_capture_take(&stdout_capture);
    char *stderr_text = worker_capture_take(&stderr_capture);
    if (wait_status != 0) {
        agent_buf message = {0};
        agent_buf_puts(&message, "subprocess agent failed: ");
        agent_buf_puts(&message, error[0] ? error : "unknown error");
        if (stderr_text[0]) {
            agent_buf_puts(&message, "\n");
            agent_buf_puts(&message, stderr_text);
        }
        free(stdout_text);
        free(stderr_text);
        char *detail = agent_buf_take(&message);
        char *result = worker_subprocess_tool_error(detail);
        free(detail);
        return result;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        free(stderr_text);
        if (stdout_text[0]) return stdout_text;
        free(stdout_text);
        return xstrdup("Subprocess completed without output.\n");
    }

    agent_buf failure = {0};
    if (WIFEXITED(status)) {
        char status_text[96];
        snprintf(status_text, sizeof(status_text),
                 "Tool error: subprocess exited with status %d\n",
                 WEXITSTATUS(status));
        agent_buf_puts(&failure, status_text);
    } else if (WIFSIGNALED(status)) {
        char signal_text[96];
        snprintf(signal_text, sizeof(signal_text),
                 "Tool error: subprocess terminated by signal %d\n",
                 WTERMSIG(status));
        agent_buf_puts(&failure, signal_text);
    } else {
        agent_buf_puts(&failure,
                       "Tool error: subprocess did not complete\n");
    }
    if (stderr_text[0]) {
        agent_buf_puts(&failure, "stderr:\n");
        agent_buf_puts(&failure, stderr_text);
        if (stderr_text[strlen(stderr_text) - 1] != '\n')
            agent_buf_puts(&failure, "\n");
    }
    if (stdout_text[0]) {
        agent_buf_puts(&failure, "stdout:\n");
        agent_buf_puts(&failure, stdout_text);
        if (stdout_text[strlen(stdout_text) - 1] != '\n')
            agent_buf_puts(&failure, "\n");
    }
    free(stdout_text);
    free(stderr_text);
    return agent_buf_take(&failure);
}

static char *worker_execute_external_tool(agent_worker *worker,
                                          const agent_tool_call *call);

static worker_native_result *worker_native_execute_calls(
        agent_worker *w, const worker_gen_ctx *generation) {
    size_t count = generation->tool_call_count;
    worker_native_result *results = xmalloc(count * sizeof(*results));
    memset(results, 0, count * sizeof(*results));
    for (size_t i = 0; i < count; i++) {
        char parse_error[160] = {0};
        results[i].arguments_valid = worker_native_call_convert(
            &generation->tool_calls[i], &results[i].call,
            parse_error, sizeof(parse_error));
        worker_native_display_call(w, &results[i].call,
                                   results[i].arguments_valid);
        if (results[i].arguments_valid) {
            results[i].result = worker_execute_external_tool(
                w, &results[i].call);
            if (!strcmp(results[i].call.name, "bash") ||
                !strcmp(results[i].call.name, "bash_status") ||
                !strcmp(results[i].call.name, "bash_stop"))
                agent_publish_tool_observation(w, results[i].result);
        } else {
            agent_buf error = {0};
            agent_buf_puts(&error, "Tool error: invalid arguments for ");
            agent_buf_puts(&error, generation->tool_calls[i].name);
            agent_buf_puts(&error, ": ");
            agent_buf_puts(&error, parse_error[0] ? parse_error :
                           "arguments must be a flat JSON object");
            agent_buf_puts(&error, "\n");
            results[i].result = agent_buf_take(&error);
        }
        if (!results[i].result)
            results[i].result = xstrdup("Tool error: empty tool result\n");
    }
    return results;
}

static char *worker_execute_external_tool(agent_worker *w,
                                          const agent_tool_call *call) {
    if (!w || !w->cfg || !call || !call->name)
        return xstrdup("Tool error: tool runtime is unavailable\n");
    if (!strcmp(call->name, "subprocess"))
        return worker_execute_subprocess_tool(w, call);
    if (!w->cfg->external_tools)
        return xstrdup("Tool error: container runner is unavailable\n");

    size_t argument_count = call->argc > 0 ? (size_t)call->argc : 0;
    sirio_container_argument *arguments = argument_count ?
        xmalloc(argument_count * sizeof(*arguments)) : NULL;
    for (size_t i = 0; i < argument_count; i++) {
        arguments[i] = (sirio_container_argument){
            .name = call->args[i].name,
            .value = call->args[i].value,
            .is_string = call->args[i].is_string,
        };
    }

    char error[256] = {0};
    char *result = NULL;
    int status = sirio_container_call(
        w->cfg->external_tools, call->name, arguments, argument_count,
        &result, error, sizeof(error));
    free(arguments);
    if (status == 0) return result;

    agent_buf output = {0};
    agent_buf_puts(&output, "Tool error: container runner failed: ");
    agent_buf_puts(&output, error[0] ? error : "runner is unavailable");
    agent_buf_puts(&output, "\n");
    return agent_buf_take(&output);
}

static size_t worker_native_continuation_chars(
        const worker_gen_ctx *generation,
        const worker_native_result *results) {
    size_t chars = generation->gen_text && generation->gen_text->ptr ?
                   generation->gen_text->len : 0;
    chars = sirio_size_add_clamp(chars, generation->reasoning_text.len);
    for (size_t i = 0; i < generation->tool_call_count; i++) {
        const sirio_tool_call *call = &generation->tool_calls[i];
        chars = sirio_size_add_clamp(chars, strlen(call->id));
        chars = sirio_size_add_clamp(chars, strlen(call->name));
        chars = sirio_size_add_clamp(chars, strlen(call->arguments_json));
        /* Every native call needs one correlated tool message, even before
         * its output size is known. */
        chars = sirio_size_add_clamp(chars, strlen(call->id));
        if (results && results[i].result)
            chars = sirio_size_add_clamp(chars,
                                         strlen(results[i].result));
    }
    return chars;
}

static bool worker_native_continuation_fits(
        agent_worker *w, const worker_gen_ctx *generation,
        const worker_native_result *results, int reserve_tokens,
        int *projected_out) {
    size_t chars = worker_native_continuation_chars(generation, results);
    size_t estimate_size = chars / 4 + (chars % 4 != 0);
    int estimate = estimate_size > (size_t)INT_MAX ?
                   INT_MAX : (int)estimate_size;
    int projected = w->context_used;
    if (estimate > INT_MAX - projected) projected = INT_MAX;
    else projected += estimate;
    if (projected_out) *projected_out = projected;
    int context = agent_worker_effective_ctx_size(w);
    return context > 0 && projected < context &&
           reserve_tokens < context - projected;
}

static void worker_native_replace_large_results(
        worker_native_result *results, size_t count) {
    static const char replacement[] =
        "Tool completed, but its full output was omitted because the native "
        "tool continuation exceeded Sirio's context budget. Retry with a "
        "smaller read/search/bash output if the details are still needed.\n";
    for (size_t i = 0; i < count; i++) {
        free(results[i].result);
        results[i].result = xstrdup(replacement);
    }
}

static void worker_native_commit_exchange(
        const worker_gen_ctx *generation,
        const worker_native_result *results) {
    conv_append_assistant_native(
        generation->gen_text && generation->gen_text->ptr ?
            generation->gen_text->ptr : "",
        generation->saw_reasoning ?
            (generation->reasoning_text.ptr ?
                generation->reasoning_text.ptr : "") : NULL,
        generation->provider_state_json.ptr,
        generation->tool_calls, generation->tool_call_count);
    for (size_t i = 0; i < generation->tool_call_count; i++)
        conv_append_tool(generation->tool_calls[i].id, results[i].result);
}

static int worker_run_turn(agent_worker *w, const char *user_text) {
    agent_config *cfg = w->cfg;
    pthread_mutex_lock(&w->mu);
    w->interrupt = false;
    w->status.error[0] = '\0';
    agent_wake_locked(w);
    pthread_mutex_unlock(&w->mu);

    char compact_err[160] = {0};
    if (!agent_worker_compact_if_needed(w, "soft limit before user turn",
                                        compact_err, sizeof(compact_err)))
    {
        if (agent_err_is_interrupted(compact_err)) {
            worker_clear_interrupt(w);
            agent_set_status(w, AGENT_WORKER_IDLE);
            return 0;
        }
        agent_set_error(w, compact_err[0] ? compact_err : "context compaction failed");
        return 1;
    }
    agent_worker_maybe_append_datetime_context(w);
    agent_trace_text(w, "user", user_text ? user_text : "",
                     user_text ? strlen(user_text) : 0);
    if (!w->session_title) {
        const char *src = user_text ? user_text : "";
        size_t n = strlen(src);
        if (n > 60) n = 60;
        w->session_title = xstrndup(src, n);
        w->session_created_at = (uint64_t)time(NULL);
    }
    if (sirio_conv.len == 0 || sirio_conv.v[0].role != 0)
        agent_worker_reset_to_sysprompt(w, compact_err, sizeof(compact_err));
    conv_append(1, user_text ? user_text : "");
    transcript_sync(w);

    pthread_mutex_lock(&w->mu);
    w->user_activity = true;
    w->session_dirty = true;
    agent_wake_locked(w);
    pthread_mutex_unlock(&w->mu);

    /* No artificial tool-round ceiling, like upstream: context pressure,
     * compaction, Ctrl+C and the final answer are the stopping conditions. */
    for (int tool_round = 0; ; tool_round++) {
        if (tool_round > 0 &&
            !agent_worker_compact_if_needed(w, "soft limit before tool continuation",
                                            compact_err, sizeof(compact_err)))
        {
            if (agent_err_is_interrupted(compact_err)) {
                worker_clear_interrupt(w);
                agent_set_status(w, AGENT_WORKER_IDLE);
                return 0;
            }
            agent_set_error(w, compact_err[0] ? compact_err : "context compaction failed");
            return 1;
        }
        agent_worker_maybe_append_system_prompt_reminder(w);

        bool use_color = isatty(STDOUT_FILENO) != 0;
        agent_token_renderer renderer = {
            .engine = w->engine,
            .worker = w,
            .format_thinking = cfg->gen.think_mode != SIRIO_THINK_NONE,
            .format_markdown = use_color,
            .in_think = false,
            .use_color = use_color,
            .last_output_newline = true,
        };
        pthread_mutex_lock(&w->mu);
        w->status.state = AGENT_WORKER_GENERATING;
        w->status.greedy_sampling = false;
        w->status.generated = 0;
        w->status.gen_tps = 0.0;
        agent_wake_locked(w);
        pthread_mutex_unlock(&w->mu);

        agent_buf gen_text = {0};
        worker_gen_ctx gctx = {
            .w = w,
            .renderer = &renderer,
            .gen_text = &gen_text,
            .enable_tools = true,
        };
        char err[160];
        if (worker_generate(w, &gctx, err, sizeof(err)) != 0) {
            renderer_finish(&renderer);
            worker_gen_ctx_free(&gctx);
            agent_buf_release(&gen_text);
            if (worker_should_interrupt(w)) {
                worker_clear_interrupt(w);
                agent_set_status(w, AGENT_WORKER_IDLE);
                return 0;
            }
            agent_set_error(w, err);
            return 1;
        }

        bool interrupted = worker_should_interrupt(w);
        renderer_finish(&renderer);
        if (interrupted) {
            worker_gen_ctx_free(&gctx);
            agent_buf_release(&gen_text);
            agent_publish_system_status(w, "Stopped by user");
            worker_clear_interrupt(w);
            agent_set_status(w, AGENT_WORKER_IDLE);
            return 0;
        }

        if (!gctx.tool_call_count) {
            conv_append(SIRIO_ROLE_ASSISTANT,
                        gen_text.ptr ? gen_text.ptr : "");
            transcript_sync(w);
            worker_gen_ctx_free(&gctx);
            agent_buf_release(&gen_text);
            agent_set_status(w, AGENT_WORKER_IDLE);
            return 0;
        }

        int projected_tokens = 0;
        int result_reserve = worker_tool_result_reserve_tokens(w);
        /* Compact before side effects if the immutable call metadata plus a
         * useful result reserve cannot fit. This avoids executing a call that
         * cannot be durably represented in the next provider request. */
        if (!worker_native_continuation_fits(w, &gctx, NULL,
                                             result_reserve,
                                             &projected_tokens)) {
            if (!agent_worker_compact(w, "native tool call needs context",
                                      compact_err, sizeof(compact_err))) {
                worker_gen_ctx_free(&gctx);
                agent_buf_release(&gen_text);
                if (agent_err_is_interrupted(compact_err)) {
                    worker_clear_interrupt(w);
                    agent_set_status(w, AGENT_WORKER_IDLE);
                    return 0;
                }
                agent_set_error(w, compact_err[0] ? compact_err :
                                "context compaction failed");
                return 1;
            }
            if (!worker_native_continuation_fits(w, &gctx, NULL, 16,
                                                 &projected_tokens)) {
                worker_gen_ctx_free(&gctx);
                agent_buf_release(&gen_text);
                agent_set_error(w,
                                "native tool call metadata exceeds context");
                return 1;
            }
        }

        worker_native_result *results = worker_native_execute_calls(w, &gctx);

        if (!worker_native_continuation_fits(w, &gctx, results,
                                             result_reserve,
                                             &projected_tokens)) {
            if (!agent_worker_compact(w, "tool result would exceed context",
                                      compact_err, sizeof(compact_err)))
            {
                /* The tools have already run. Preserve a valid correlated
                 * exchange with bounded observations so a retry cannot
                 * silently repeat side effects. */
                worker_native_replace_large_results(results,
                                                    gctx.tool_call_count);
                worker_native_commit_exchange(&gctx, results);
                transcript_sync(w);
                worker_native_results_free(results, gctx.tool_call_count);
                worker_gen_ctx_free(&gctx);
                agent_buf_release(&gen_text);
                if (agent_err_is_interrupted(compact_err)) {
                    worker_clear_interrupt(w);
                    agent_set_status(w, AGENT_WORKER_IDLE);
                    return 0;
                }
                agent_set_error(w, compact_err[0] ? compact_err : "context compaction failed");
                return 1;
            }
            if (!worker_native_continuation_fits(w, &gctx, results,
                                                 result_reserve,
                                                 &projected_tokens)) {
                worker_native_replace_large_results(results,
                                                    gctx.tool_call_count);
                if (!worker_native_continuation_fits(w, &gctx, results,
                                                     16, &projected_tokens)) {
                    worker_native_commit_exchange(&gctx, results);
                    transcript_sync(w);
                    worker_native_results_free(results,
                                               gctx.tool_call_count);
                    worker_gen_ctx_free(&gctx);
                    agent_buf_release(&gen_text);
                    agent_set_error(w, "context full after compaction");
                    return 1;
                }
            }
        }

        worker_native_commit_exchange(&gctx, results);
        transcript_sync(w);
        worker_native_results_free(results, gctx.tool_call_count);
        worker_gen_ctx_free(&gctx);
        agent_buf_release(&gen_text);

        char *queued_user = worker_request_queued_user_drain(w);
        if (queued_user && queued_user[0]) {
            agent_trace_text(w, "queued_user", queued_user, strlen(queued_user));
            conv_append(1, queued_user);
            transcript_sync(w);
            pthread_mutex_lock(&w->mu);
            w->user_activity = true;
            w->session_dirty = true;
            agent_wake_locked(w);
            pthread_mutex_unlock(&w->mu);
        }
        free(queued_user);
    }
}

static int worker_run_raw_prompt(agent_worker *w, const char *user_text) {
    agent_config *cfg = w->cfg;
    pthread_mutex_lock(&w->mu);
    w->interrupt = false;
    w->status.error[0] = '\0';
    agent_wake_locked(w);
    pthread_mutex_unlock(&w->mu);

    agent_trace_text(w, "raw-prompt", user_text ? user_text : "",
                     user_text ? strlen(user_text) : 0);
    conv_clear();
    conv_append(1, user_text ? user_text : "");
    transcript_sync(w);

    pthread_mutex_lock(&w->mu);
    w->status.state = AGENT_WORKER_GENERATING;
    w->status.greedy_sampling = cfg->gen.think_mode == SIRIO_THINK_NONE &&
                                cfg->gen.temperature <= 0.0f;
    w->status.generated = 0;
    w->status.gen_tps = 0.0;
    agent_wake_locked(w);
    pthread_mutex_unlock(&w->mu);

    agent_buf gen_text = {0};
    worker_gen_ctx gctx = {
        .w = w,
        .gen_text = &gen_text,
        .raw_output = true,
    };
    char err[160];
    int rc = worker_generate(w, &gctx, err, sizeof(err));
    if (rc != 0) {
        worker_gen_ctx_free(&gctx);
        agent_buf_release(&gen_text);
        if (worker_should_interrupt(w)) {
            worker_clear_interrupt(w);
            agent_set_status(w, AGENT_WORKER_IDLE);
            return 0;
        }
        agent_set_error(w, err);
        return 1;
    }
    conv_append(2, gen_text.ptr ? gen_text.ptr : "");
    transcript_sync(w);
    worker_gen_ctx_free(&gctx);
    agent_buf_release(&gen_text);

    if (worker_should_interrupt(w)) {
        worker_clear_interrupt(w);
        agent_set_status(w, AGENT_WORKER_IDLE);
        return 0;
    }
    agent_set_status(w, AGENT_WORKER_IDLE);
    return 0;
}

static void *worker_main(void *arg) {
    agent_worker *w = arg;
    agent_trace(w, "agent worker start ctx=%d backend=%s",
                agent_worker_effective_ctx_size(w), "sirio-bridge");
    char init_err[160] = {0};
    bool init_ok = true;
    if (init_ok && w->cfg->resume_session)
        init_ok = agent_worker_load_session(
            w, w->cfg->resume_session, w->cfg->resume_history_turns,
            false, init_err, sizeof(init_err));
    else if (init_ok && !w->cfg->gen.raw_prompt)
        init_ok = agent_worker_reset_to_sysprompt(w, init_err, sizeof(init_err));
    if (!init_ok) {
        agent_set_error(w, init_err[0] ? init_err : "failed to initialize system prompt");
    }
    if (w->cfg->gen.raw_prompt)
        agent_trace(w, "raw one-shot prompt mode: skipping agent system prompt");
    pthread_mutex_lock(&w->mu);
    w->initialized = true;
    agent_wake_locked(w);
    pthread_mutex_unlock(&w->mu);

    while (true) {
        pthread_mutex_lock(&w->mu);
        while (!w->stop && !w->cmd_text && !w->save_requested &&
               !w->compact_requested)
            pthread_cond_wait(&w->cond, &w->mu);
        if (w->stop) {
            pthread_mutex_unlock(&w->mu);
            break;
        }
        if (!w->cmd_text && w->save_requested) {
            pthread_mutex_unlock(&w->mu);
            worker_run_deferred_save(w);
            continue;
        }
        if (!w->cmd_text && w->compact_requested) {
            pthread_mutex_unlock(&w->mu);
            worker_run_deferred_compact(w);
            continue;
        }
        char *cmd = w->cmd_text;
        w->cmd_text = NULL;
        pthread_mutex_unlock(&w->mu);

        if (w->cfg->gen.raw_prompt)
            worker_run_raw_prompt(w, cmd);
        else
            worker_run_turn(w, cmd);
        free(cmd);
        worker_run_deferred_compact(w);
        worker_run_deferred_save(w);
    }

    agent_set_status(w, AGENT_WORKER_STOPPED);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Adapted functions called by the kept code.                           */
/* ------------------------------------------------------------------ */

int agent_worker_effective_ctx_size(const agent_worker *w) {
    if (w && w->cfg && w->cfg->gen.ctx_size > 0) return w->cfg->gen.ctx_size;
    return 65536;
}

typedef struct {
    agent_buf summary;
    char err[256];
    bool failed;
} worker_compact_ctx;

typedef int (*worker_generate_options_fn)(
    sirio_bridge *, const sirio_generation_options *,
    const sirio_message *, size_t, const sirio_tool *, size_t,
    sirio_bridge_event_callback, void *);

#ifdef SIRIO_CORE_TEST
static worker_generate_options_fn sirio_test_generate_with_options;
#endif

static int worker_generate_with_options(
        sirio_bridge *bridge, const sirio_generation_options *options,
        const sirio_message *messages, size_t message_count,
        const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
#ifdef SIRIO_CORE_TEST
    if (sirio_test_generate_with_options)
        return sirio_test_generate_with_options(
            bridge, options, messages, message_count, tools, tool_count,
            callback, private_data);
#endif
    return sirio_bridge_generate_with_options(
        bridge, options, messages, message_count, tools, tool_count,
        callback, private_data);
}

static int worker_compact_event(const sirio_bridge_event *event,
                                void *private_data) {
    worker_compact_ctx *ctx = private_data;
    if (!event) return 0;
    if (event->type == SIRIO_BRIDGE_EVENT_TEXT && event->text) {
        agent_buf_puts(&ctx->summary, event->text);
    } else if (event->type == SIRIO_BRIDGE_EVENT_TOOL_CALL) {
        worker_copy_err(ctx->err, sizeof(ctx->err),
                        "compaction summary attempted a tool call");
        ctx->failed = true;
        return 1;
    } else if (event->type == SIRIO_BRIDGE_EVENT_ERROR) {
        worker_copy_err(ctx->err, sizeof(ctx->err),
                        event->error ? event->error :
                        "compaction generation failed");
        ctx->failed = true;
        return 1;
    }
    return 0;
}

/* Message-level equivalent of the upstream token-tail selector. */
static int agent_compact_tail_start(agent_worker *w, int bottom, int sys_len) {
    int ctx = agent_worker_effective_ctx_size(w);
    int tail_budget = ctx / AGENT_COMPACT_TAIL_DIVISOR;
    if (tail_budget > AGENT_COMPACT_TAIL_CAP_TOKENS)
        tail_budget = AGENT_COMPACT_TAIL_CAP_TOKENS;
    if (tail_budget < 1) tail_budget = 1;

    int target = bottom;
    int used = 0;
    for (int i = bottom - 1; i >= sys_len; i--) {
        size_t chars = conv_msg_chars(&sirio_conv.v[i]);
        size_t estimate_size = chars / 4 + (chars % 4 != 0);
        int estimate = estimate_size > (size_t)INT_MAX ?
                       INT_MAX : (int)estimate_size;
        if (target < bottom && (estimate > tail_budget - used)) break;
        target = i;
        if (estimate > tail_budget - used) {
            used = tail_budget;
            break;
        }
        used += estimate;
    }
    if (target == bottom && bottom > sys_len) target = bottom - 1;
    for (int i = target; i < bottom; i++) {
        if (sirio_conv.v[i].role == SIRIO_ROLE_USER) return i;
    }
    /* Never retain a suffix that starts in the middle of an assistant native
     * call followed by its tool-result messages. */
    while (target > sys_len &&
           sirio_conv.v[target].role == SIRIO_ROLE_TOOL)
        target--;
    return target;
}

static void conv_compact_commit(const char *summary,
                                int tail_start, int bottom) {
    int keep_system = sirio_conv.len > 0 && sirio_conv.v[0].role == 0;
    if (tail_start < keep_system) tail_start = keep_system;
    if (bottom > sirio_conv.len) bottom = sirio_conv.len;
    if (tail_start > bottom) tail_start = bottom;

    agent_buf wrapped = {0};
    agent_buf_puts(&wrapped,
        "\n\n[Sirio compacted earlier conversation. Durable task-state summary follows.]\n");
    agent_buf_puts(&wrapped, summary);
    if (wrapped.len && wrapped.ptr[wrapped.len - 1] != '\n')
        agent_buf_puts(&wrapped, "\n");
    agent_buf_puts(&wrapped,
        "[End compacted summary. Recent conversation continues verbatim below.]\n\n");

    int new_len = keep_system + 1 + (bottom - tail_start);
    sirio_conv_msg *next = xmalloc((size_t)new_len * sizeof(*next));
    memset(next, 0, (size_t)new_len * sizeof(*next));
    int n = 0;
    if (keep_system) {
        conv_msg_clone(&next[n], &sirio_conv.v[0]);
        n++;
    }
    next[n].role = SIRIO_ROLE_SYSTEM;
    next[n].text = agent_buf_take(&wrapped);
    n++;
    for (int i = tail_start; i < bottom; i++) {
        conv_msg_clone(&next[n], &sirio_conv.v[i]);
        n++;
    }

    for (int i = 0; i < sirio_conv.len; i++)
        conv_msg_free(&sirio_conv.v[i]);
    free(sirio_conv.v);
    sirio_conv.v = next;
    sirio_conv.len = n;
    sirio_conv.cap = new_len;
    conv_recount_chars();
    sirio_conv.provider_usage_valid = false;
}

static char *worker_compact_summary_take(worker_compact_ctx *ctx) {
    char *summary = agent_buf_take(&ctx->summary);
    if (!summary) return NULL;
    size_t start = 0;
    size_t end = strlen(summary);
    while (start < end && isspace((unsigned char)summary[start])) start++;
    while (end > start && isspace((unsigned char)summary[end - 1])) end--;
    if (start) memmove(summary, summary + start, end - start);
    summary[end - start] = '\0';
    return summary;
}

bool agent_worker_compact(agent_worker *w, const char *reason,
                          char *err, size_t err_len) {
    const int bottom = sirio_conv.len;
    if (bottom <= 1) return true;
    if (!sirio_conv.bridge) {
        worker_copy_err(err, err_len, "bridge is not initialized");
        return false;
    }

    char *prompt = agent_compact_make_prompt(reason);
    int message_count = 0;
    sirio_message *messages = conv_build_messages(&message_count);
    messages = xrealloc(messages,
                        (size_t)(message_count + 1) * sizeof(*messages));
    memset(&messages[message_count], 0, sizeof(messages[message_count]));
    messages[message_count].role = SIRIO_ROLE_USER;
    messages[message_count].content = prompt;
    message_count++;

    agent_publishf(w,
        "\n\x1b[1;95mCOMPACTING\x1b[0m %s: summarizing durable task state\n",
        reason && reason[0] ? reason : "context");
    pthread_mutex_lock(&w->mu);
    w->status.state = AGENT_WORKER_COMPACTING;
    w->status.generated = 0;
    w->status.gen_tps = 0.0;
    w->status.greedy_sampling = false;
    agent_wake_locked(w);
    pthread_mutex_unlock(&w->mu);

    sirio_generation_options options = {
        .max_tokens = AGENT_COMPACT_SUMMARY_MAX_TOKENS,
        .temperature = -1.0,
        .top_p = -1.0,
        .reasoning = SIRIO_REASONING_NONE,
    };
    worker_compact_ctx compact = {0};
    int rc = worker_generate_with_options(
        sirio_conv.bridge, &options, messages, (size_t)message_count,
        NULL, 0, worker_compact_event, &compact);
    free(messages);
    free(prompt);
    if (rc != 0 || compact.failed) {
        if (worker_should_interrupt(w))
            worker_copy_err(err, err_len, "interrupted");
        else
            worker_copy_err(err, err_len,
                            compact.err[0] ? compact.err :
                            sirio_bridge_last_error(sirio_conv.bridge));
        agent_buf_release(&compact.summary);
        return false;
    }

    char *summary = worker_compact_summary_take(&compact);
    if (!summary || !summary[0]) {
        free(summary);
        worker_copy_err(err, err_len, "compaction summary was empty");
        return false;
    }
    int sys_len = bottom > 0 && sirio_conv.v[0].role == 0 ? 1 : 0;
    int tail_start = agent_compact_tail_start(w, bottom, sys_len);
    conv_compact_commit(summary, tail_start, bottom);
    free(summary);
    transcript_sync(w);
    agent_worker_note_system_prompt_seen(w);
    w->session_dirty = true;

    agent_publishf(w,
                   "\x1b[1;95mCOMPACTING\x1b[0m rebuilt context: "
                   "old_messages=%d new_messages=%d tail_messages=%d\n",
                   bottom, sirio_conv.len, bottom - tail_start);
    agent_trace(w,
                "compacted reason=\"%s\" old_messages=%d new_messages=%d "
                "tail_start=%d tail_messages=%d",
                reason ? reason : "", bottom, sirio_conv.len,
                tail_start, bottom - tail_start);
    return true;
}

static char *sirio_build_system_message(const agent_worker *w) {
    agent_buf b = {0};
    char *tools = sirio_build_native_tools_prompt(w->cfg->edit_upto);
    if (tools) {
        agent_buf_puts(&b, tools);
        free(tools);
    }
    if (w->cfg->gen.system && w->cfg->gen.system[0]) {
        agent_buf_puts(&b, "\n\n");
        agent_buf_puts(&b, w->cfg->gen.system);
    }
    return agent_buf_take(&b);
}

bool agent_worker_reset_to_sysprompt(agent_worker *w,
                                     char *err, size_t err_len) {
    (void)err; (void)err_len;
    conv_clear();
    char *text = sirio_build_system_message(w);
    conv_append(0, text);
    free(text);
    transcript_sync(w);
    agent_worker_note_system_prompt_seen(w);
    return true;
}

static void agent_worker_publish_assistant_history(agent_worker *w,
                                                   const char *text,
                                                   bool use_color) {
    if (!text || !text[0]) {
        agent_publishf(w, "assistant:\n");
        return;
    }
    agent_publishf(w, "assistant: ");
    agent_token_renderer renderer = {
        .engine = w->engine,
        .worker = w,
        .format_thinking = true,
        .format_markdown = use_color,
        .use_color = use_color,
        .last_output_newline = true,
    };
    renderer_process(&renderer, text, strlen(text), false);
    renderer_finish(&renderer);
    if (!renderer.wrote_visible_output) agent_publishf(w, "\n");
}

bool agent_worker_show_history(agent_worker *w, int user_turns,
                               char *err, size_t err_len) {
    (void)err; (void)err_len;
    /* Find the start of the last user_turns user turns; user_turns <= 0
     * prints the whole conversation including the system prompt. */
    int start = 0;
    if (user_turns > 0) {
        int seen = 0;
        start = 1; /* never print the system prompt in the windowed view */
        for (int i = sirio_conv.len - 1; i >= 1; i--) {
            if (sirio_conv.v[i].role != 1) continue;
            if (++seen == user_turns) {
                start = i;
                break;
            }
        }
        if (seen < user_turns) start = 1; /* fewer turns than requested */
    }
    for (int i = start; i < sirio_conv.len; i++) {
        const sirio_conv_msg *message = &sirio_conv.v[i];
        const char *label = message->role == SIRIO_ROLE_SYSTEM ? "system" :
                            message->role == SIRIO_ROLE_USER ? "user" :
                            message->role == SIRIO_ROLE_ASSISTANT ?
                                "assistant" : "tool";
        if (message->role == SIRIO_ROLE_TOOL)
            agent_publishf(w, "%s[%s]: %s\n", label,
                           message->tool_call_id ? message->tool_call_id : "?",
                           message->text ? message->text : "");
        else if (message->role == SIRIO_ROLE_ASSISTANT)
            agent_worker_publish_assistant_history(
                w, message->text, isatty(STDOUT_FILENO) != 0);
        else
            agent_publishf(w, "%s: %s\n", label,
                           message->text ? message->text : "");
        for (size_t j = 0; j < message->tool_call_count; j++) {
            const sirio_tool_call *call = &message->tool_calls[j];
            agent_publishf(w, "tool-call[%s]: %s %s\n", call->id,
                           call->name, call->arguments_json);
        }
    }
    return true;
}

static void conv_drop_provider_cache(void) {
    for (int i = 0; i < sirio_conv.len; i++) {
        free(sirio_conv.v[i].provider_state_json);
        sirio_conv.v[i].provider_state_json = NULL;
    }
    sirio_conv.provider_anchor_chars = 0;
    sirio_conv.provider_prompt_tokens = 0;
    sirio_conv.provider_usage_valid = false;
    conv_recount_chars();
}

static void agent_worker_finish_model_change(agent_worker *w,
                                              const sirio_model_info *old_model) {
    if (old_model != w->engine->model) conv_drop_provider_cache();
    sirio_conv.bridge = w->engine->bridge;
    sirio_conv.provider = w->engine->provider;
    w->cfg->gen.think_mode = (sirio_think_mode)w->engine->reasoning;
    if (w->engine->model && w->engine->model->context_tokens > 0)
        w->cfg->gen.ctx_size = w->engine->model->context_tokens;
    pthread_mutex_lock(&w->mu);
    worker_status_selection_locked(w);
    transcript_sync(w);
    agent_wake_locked(w);
    pthread_mutex_unlock(&w->mu);
}

static bool agent_worker_select_model_internal(agent_worker *w,
                                               const char *model,
                                               const char *reasoning,
                                               char *err, size_t err_len) {
    if (!w || !w->engine || !w->engine->select) {
        worker_copy_err(err, err_len, "runtime model selection is unavailable");
        return false;
    }
    if (!worker_is_idle(w)) {
        worker_copy_err(err, err_len, "model can only change while idle");
        return false;
    }
    if (!model || !model[0]) {
        worker_copy_err(err, err_len, "model name is empty");
        return false;
    }

    const sirio_model_info *old_model = w->engine->model;
    if (w->engine->select(w->engine, model, reasoning, err, err_len) != 0)
        return false;
    agent_worker_finish_model_change(w, old_model);
    return true;
}

bool agent_worker_select_model(agent_worker *w, const char *model,
                               const char *reasoning,
                               char *err, size_t err_len) {
    return agent_worker_select_model_internal(
        w, model, reasoning, err, err_len);
}

bool agent_worker_step_model(agent_worker *w, int direction,
                             bool *at_limit,
                             char *err, size_t err_len) {
    if (at_limit) *at_limit = false;
    if (!w || !w->engine || !w->engine->step_model) {
        worker_copy_err(err, err_len, "runtime model selection is unavailable");
        return false;
    }
    if (!worker_is_idle(w)) {
        worker_copy_err(err, err_len, "model can only change while idle");
        return false;
    }
    const sirio_model_info *old_model = w->engine->model;
    int status = w->engine->step_model(w->engine, direction, at_limit,
                                       err, err_len);
    if (status != 0) return false;
    if (at_limit && *at_limit) return true;
    agent_worker_finish_model_change(w, old_model);
    return true;
}

bool agent_worker_step_reasoning(agent_worker *w, int direction,
                                 bool *at_limit,
                                 char *err, size_t err_len) {
    if (at_limit) *at_limit = false;
    if (!w || !w->engine || !w->engine->model) {
        worker_copy_err(err, err_len, "inference model is unavailable");
        return false;
    }
    sirio_reasoning_effort next = w->engine->reasoning;
    if (!sirio_model_step_reasoning(w->engine->model, w->engine->reasoning,
                                    direction, &next)) {
        if (at_limit) *at_limit = true;
        return true;
    }
    return agent_worker_select_model(
        w, w->engine->model->name, sirio_reasoning_name(next),
        err, err_len);
}

/* Runtime help is user-visible engine behavior: keep it at the adapter seam
 * rather than retaining upstream local-GPU/KV wording in the portable core. */
void runtime_help(void) {
    (void)sirio_help_print(stdout, "commands-internal");
}

/* Cloud status must not present unavailable local prefill rates, sampled-token
 * rates or local KV state as real measurements. */
void build_status_text(const agent_status *st, char *buf, size_t len) {
    char used[32];
    char total_ctx[32];
    agent_format_ctx_size(st->ctx_used, used, sizeof(used));
    agent_format_ctx_size(st->ctx_size, total_ctx, sizeof(total_ctx));

    const char *state = "idle";
    switch (st->state) {
    case AGENT_WORKER_PREFILL: state = "waiting"; break;
    case AGENT_WORKER_GENERATING: state = "streaming"; break;
    case AGENT_WORKER_COMPACTING: state = "compacting"; break;
    case AGENT_WORKER_DRAINING: state = "stopping"; break;
    case AGENT_WORKER_SAVING: state = "saving"; break;
    case AGENT_WORKER_ERROR: state = st->error[0] ? st->error : "error"; break;
    case AGENT_WORKER_STOPPED: state = "interrupted"; break;
    default: break;
    }
    snprintf(buf, len, "%s · %s | ctx %s/%s | %s",
             st->model[0] ? st->model : "model",
             st->reasoning[0] ? st->reasoning : "thinking",
             used, total_ctx, state);
}

/* ------------------------------------------------------------------ */
/* Portable cloud transcript persistence.                              */
/* ------------------------------------------------------------------ */

#define SIRIO_SESSION_MAGIC "SIRIO_SESSION"
#define SIRIO_SESSION_VERSION 2
#define SIRIO_SESSION_EXT ".sirio"
#define SIRIO_SESSION_MAX_BYTES (128u * 1024u * 1024u)
#define SIRIO_SESSION_MAX_FILE_BYTES \
    (SIRIO_SESSION_MAX_BYTES + 32u * 1024u * 1024u)
#define SIRIO_SESSION_MAX_MESSAGES 100000u
#define SIRIO_SESSION_MAX_TOOL_CALLS 100000u
#define SIRIO_SESSION_MAX_TOOL_CALLS_PER_MESSAGE 1024u
#define SIRIO_SESSION_MAX_TITLE_BYTES (64u * 1024u)

typedef struct {
    uint32_t h[5];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
} sirio_sha1_ctx;

static uint32_t sirio_sha1_rol(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32 - shift));
}

static void sirio_sha1_transform(sirio_sha1_ctx *ctx,
                                 const unsigned char block[64]) {
    uint32_t words[80];
    for (int i = 0; i < 16; i++) {
        const unsigned char *p = block + i * 4;
        words[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    for (int i = 16; i < 80; i++)
        words[i] = sirio_sha1_rol(words[i - 3] ^ words[i - 8] ^
                                  words[i - 14] ^ words[i - 16], 1);

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2];
    uint32_t d = ctx->h[3], e = ctx->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = UINT32_C(0x5a827999);
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = UINT32_C(0x6ed9eba1);
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = UINT32_C(0x8f1bbcdc);
        } else {
            f = b ^ c ^ d;
            k = UINT32_C(0xca62c1d6);
        }
        uint32_t next = sirio_sha1_rol(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = sirio_sha1_rol(b, 30);
        b = a;
        a = next;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sirio_sha1_init(sirio_sha1_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->h[0] = UINT32_C(0x67452301);
    ctx->h[1] = UINT32_C(0xefcdab89);
    ctx->h[2] = UINT32_C(0x98badcfe);
    ctx->h[3] = UINT32_C(0x10325476);
    ctx->h[4] = UINT32_C(0xc3d2e1f0);
}

static void sirio_sha1_update(sirio_sha1_ctx *ctx, const void *data,
                              size_t len) {
    const unsigned char *p = data;
    ctx->bytes += len;
    while (len) {
        size_t take = sizeof(ctx->block) - ctx->used;
        if (take > len) take = len;
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == sizeof(ctx->block)) {
            sirio_sha1_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sirio_sha1_final(sirio_sha1_ctx *ctx, unsigned char digest[20]) {
    uint64_t bits = ctx->bytes * 8;
    const unsigned char marker = 0x80;
    unsigned char zeros[64] = {0};
    sirio_sha1_update(ctx, &marker, 1);
    size_t padding = ctx->used <= 56 ? 56 - ctx->used : 120 - ctx->used;
    sirio_sha1_update(ctx, zeros, padding);
    unsigned char length[8];
    for (int i = 0; i < 8; i++)
        length[7 - i] = (unsigned char)(bits >> (i * 8));
    sirio_sha1_update(ctx, length, sizeof(length));
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (unsigned char)(ctx->h[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(ctx->h[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(ctx->h[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)ctx->h[i];
    }
}

/* Keep the upstream stable identity contract while changing only the payload:
 * SHA1(title || created_at_le64). */
static void agent_session_identity_sha(const char *title, uint64_t created_at,
                                       char sha_out[41]) {
    sirio_sha1_ctx ctx;
    unsigned char digest[20];
    unsigned char created_le[8];
    static const char hex[] = "0123456789abcdef";
    agent_le_put64(created_le, created_at);
    sirio_sha1_init(&ctx);
    sirio_sha1_update(&ctx, title, strlen(title));
    sirio_sha1_update(&ctx, created_le, sizeof(created_le));
    sirio_sha1_final(&ctx, digest);
    for (int i = 0; i < 20; i++) {
        sha_out[i * 2] = hex[digest[i] >> 4];
        sha_out[i * 2 + 1] = hex[digest[i] & 15];
    }
    sha_out[40] = '\0';
}

typedef struct {
    char *title;
    char *provider;
    char *model;
    char *reasoning;
    uint64_t created_at;
    uint64_t last_used;
    sirio_conv_msg *messages;
    int message_count;
    int chars;
} sirio_session_data;

static bool sirio_session_validate_messages(const sirio_conv_msg *messages,
                                            int message_count,
                                            char *err, size_t err_len);

static void sirio_session_data_free(sirio_session_data *data) {
    if (!data) return;
    free(data->title);
    free(data->provider);
    free(data->model);
    free(data->reasoning);
    for (int i = 0; i < data->message_count; i++)
        if (data->messages) conv_msg_free(&data->messages[i]);
    free(data->messages);
    memset(data, 0, sizeof(*data));
}

static char *sirio_default_session_dir(void) {
    agent_buf path = {0};
    const char *home = getenv("HOME");
    agent_buf_puts(&path, home && home[0] ? home : ".");
    agent_buf_puts(&path, "/.sirio");
    if (path.len && path.ptr[path.len - 1] != '/') agent_buf_puts(&path, "/");
    agent_buf_puts(&path, "sessions");
    return agent_buf_take(&path);
}

static char *sirio_session_path(const char *cache_dir, const char sha[41]) {
    agent_buf path = {0};
    agent_buf_puts(&path, cache_dir);
    if (path.len && path.ptr[path.len - 1] != '/') agent_buf_puts(&path, "/");
    agent_buf_puts(&path, sha);
    agent_buf_puts(&path, SIRIO_SESSION_EXT);
    return agent_buf_take(&path);
}

static bool sirio_session_filename_sha(const char *name, char sha_out[41]) {
    const size_t ext_len = sizeof(SIRIO_SESSION_EXT) - 1;
    size_t len = strlen(name);
    if (len != 40 + ext_len || strcmp(name + 40, SIRIO_SESSION_EXT))
        return false;
    for (int i = 0; i < 40; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isxdigit(c)) return false;
        sha_out[i] = (char)tolower(c);
    }
    sha_out[40] = '\0';
    return true;
}

static bool sirio_session_read_line(FILE *fp, char *line, size_t line_len,
                                    char *err, size_t err_len) {
    if (!fgets(line, (int)line_len, fp)) {
        worker_copy_err(err, err_len, "truncated session header");
        return false;
    }
    size_t len = strlen(line);
    if (len && line[len - 1] == '\n') line[--len] = '\0';
    if (len && line[len - 1] == '\r') line[--len] = '\0';
    if (len + 1 == line_len && len && line[len - 1] != '\n') {
        worker_copy_err(err, err_len, "session header line is too long");
        return false;
    }
    return true;
}

static bool sirio_session_parse_u64(const char *line, const char *label,
                                    uint64_t maximum, uint64_t *value,
                                    char *err, size_t err_len) {
    size_t label_len = strlen(label);
    if (strncmp(line, label, label_len) || line[label_len] != ' ') {
        worker_copy_err(err, err_len, "invalid session header");
        return false;
    }
    const char *text = line + label_len + 1;
    if (!text[0] || text[0] == '-') {
        worker_copy_err(err, err_len, "invalid session number");
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || !end || *end || parsed > maximum) {
        worker_copy_err(err, err_len, "invalid session number");
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool sirio_session_parse_numbers(const char *line, const char *prefix,
                                        const uint64_t *maximums,
                                        uint64_t *values, size_t count,
                                        char *err, size_t err_len) {
    size_t prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len)) {
        worker_copy_err(err, err_len, "invalid session record header");
        return false;
    }
    const char *cursor = line + prefix_len;
    for (size_t i = 0; i < count; i++) {
        if (!isdigit((unsigned char)*cursor)) {
            worker_copy_err(err, err_len, "invalid session record number");
            return false;
        }
        errno = 0;
        char *end = NULL;
        unsigned long long parsed = strtoull(cursor, &end, 10);
        if (errno || end == cursor || parsed > maximums[i]) {
            worker_copy_err(err, err_len, "invalid session record number");
            return false;
        }
        values[i] = (uint64_t)parsed;
        if (i + 1 == count) {
            if (*end) {
                worker_copy_err(err, err_len,
                                "trailing data in session record header");
                return false;
            }
        } else {
            if (*end != ' ' || !end[1]) {
                worker_copy_err(err, err_len,
                                "truncated session record header");
                return false;
            }
            cursor = end + 1;
        }
    }
    return true;
}

typedef struct {
    int role;
    int provider;
    uint64_t text_bytes;
    uint64_t reasoning_bytes;
    uint64_t provider_state_bytes;
    uint64_t tool_call_id_bytes;
    uint64_t tool_call_count;
    unsigned flags;
} sirio_session_message_header;

static bool sirio_session_parse_message(
        const char *line, sirio_session_message_header *header,
        char *err, size_t err_len) {
    static const uint64_t maximums[] = {
        3, SIRIO_PROVIDER_COUNT - 1, SIRIO_SESSION_MAX_BYTES,
        SIRIO_SESSION_MAX_BYTES, SIRIO_SESSION_MAX_BYTES,
        SIRIO_SESSION_MAX_BYTES, SIRIO_SESSION_MAX_TOOL_CALLS_PER_MESSAGE, 7
    };
    uint64_t values[8] = {0};
    if (!sirio_session_parse_numbers(
            line, "message ", maximums, values, 8, err, err_len))
        return false;
    header->role = (int)values[0];
    header->provider = (int)values[1];
    header->text_bytes = values[2];
    header->reasoning_bytes = values[3];
    header->provider_state_bytes = values[4];
    header->tool_call_id_bytes = values[5];
    header->tool_call_count = values[6];
    header->flags = (unsigned)values[7];
    return true;
}

static bool sirio_session_parse_call(const char *line, uint64_t *id_bytes,
                                     uint64_t *name_bytes,
                                     uint64_t *arguments_bytes,
                                     char *err, size_t err_len) {
    static const uint64_t maximums[] = {
        SIRIO_SESSION_MAX_BYTES, SIRIO_SESSION_MAX_BYTES,
        SIRIO_SESSION_MAX_BYTES
    };
    uint64_t values[3];
    if (!sirio_session_parse_numbers(line, "call ", maximums, values, 3,
                                     err, err_len))
        return false;
    *id_bytes = values[0];
    *name_bytes = values[1];
    *arguments_bytes = values[2];
    return true;
}

static bool sirio_session_read_blob(FILE *fp, uint64_t bytes, bool keep,
                                    char **text_out,
                                    char *err, size_t err_len) {
    char *text = NULL;
    if (keep) {
        text = xmalloc((size_t)bytes + 1);
        if (bytes && fread(text, 1, (size_t)bytes, fp) != bytes) {
            free(text);
            worker_copy_err(err, err_len, "truncated session text");
            return false;
        }
        if (memchr(text, '\0', (size_t)bytes)) {
            free(text);
            worker_copy_err(err, err_len, "session text contains a NUL byte");
            return false;
        }
        text[bytes] = '\0';
    } else if (bytes && fseeko(fp, (off_t)bytes, SEEK_CUR) != 0) {
        worker_copy_err(err, err_len, "truncated session text");
        return false;
    }
    if (fgetc(fp) != '\n') {
        free(text);
        worker_copy_err(err, err_len, "invalid session text delimiter");
        return false;
    }
    if (text_out) *text_out = text;
    else free(text);
    return true;
}

static bool sirio_session_add_chars(uint64_t *total, uint64_t bytes,
                                    char *err, size_t err_len) {
    if (bytes > SIRIO_SESSION_MAX_BYTES - *total) {
        worker_copy_err(err, err_len, "session transcript is too large");
        return false;
    }
    *total += bytes;
    return true;
}

static bool sirio_session_validate_messages(const sirio_conv_msg *messages,
                                            int message_count,
                                            char *err, size_t err_len) {
    for (int i = 0; i < message_count; i++) {
        const sirio_conv_msg *message = &messages[i];
        if (message->role < SIRIO_ROLE_SYSTEM ||
            message->role > SIRIO_ROLE_TOOL ||
            message->provider < SIRIO_PROVIDER_NONE ||
            message->provider >= SIRIO_PROVIDER_COUNT || !message->text) {
            worker_copy_err(err, err_len, "invalid session message");
            return false;
        }
        if (message->role != SIRIO_ROLE_ASSISTANT &&
            message->provider != SIRIO_PROVIDER_NONE) {
            worker_copy_err(err, err_len,
                            "provider owner on a non-assistant message");
            return false;
        }
        if (message->role != SIRIO_ROLE_ASSISTANT &&
            (message->reasoning || message->provider_state_json ||
             message->tool_calls ||
             message->tool_call_count)) {
            worker_copy_err(err, err_len,
                            "native assistant fields on a non-assistant message");
            return false;
        }
        if (message->role != SIRIO_ROLE_TOOL && message->tool_call_id) {
            worker_copy_err(err, err_len,
                            "tool-call id on a non-tool message");
            return false;
        }
        if (message->role == SIRIO_ROLE_TOOL &&
            (!message->tool_call_id || !message->tool_call_id[0])) {
            worker_copy_err(err, err_len,
                            "tool message has no tool-call id");
            return false;
        }
        if (message->role == SIRIO_ROLE_ASSISTANT) {
            if ((message->tool_call_count != 0) !=
                (message->tool_calls != NULL)) {
                worker_copy_err(err, err_len,
                                "invalid assistant tool-call array");
                return false;
            }
            for (size_t j = 0; j < message->tool_call_count; j++) {
                const sirio_tool_call *call = &message->tool_calls[j];
                if (!call->id || !call->id[0] ||
                    !call->name || !call->name[0] ||
                    !call->arguments_json) {
                    worker_copy_err(err, err_len,
                                    "incomplete assistant tool call");
                    return false;
                }
                for (size_t k = 0; k < j; k++) {
                    if (!strcmp(call->id, message->tool_calls[k].id)) {
                        worker_copy_err(err, err_len,
                                        "duplicate assistant tool-call id");
                        return false;
                    }
                }
            }
        }
    }

    for (int i = 0; i < message_count; i++) {
        const sirio_conv_msg *message = &messages[i];
        if (message->role == SIRIO_ROLE_TOOL) {
            worker_copy_err(err, err_len, "orphaned tool message in session");
            return false;
        }
        if (message->role != SIRIO_ROLE_ASSISTANT ||
            !message->tool_call_count)
            continue;
        if (message->tool_call_count > (size_t)(message_count - i - 1)) {
            worker_copy_err(err, err_len,
                            "incomplete tool continuation in session");
            return false;
        }
        for (size_t j = 0; j < message->tool_call_count; j++) {
            const sirio_conv_msg *result = &messages[i + 1 + (int)j];
            if (result->role != SIRIO_ROLE_TOOL) {
                worker_copy_err(err, err_len,
                                "mismatched tool continuation in session");
                return false;
            }
            size_t matches = 0;
            for (size_t k = 0; k < message->tool_call_count; k++) {
                if (!strcmp(result->tool_call_id,
                            message->tool_calls[k].id))
                    matches++;
            }
            if (matches != 1) {
                worker_copy_err(err, err_len,
                                "mismatched tool continuation in session");
                return false;
            }
            for (size_t k = 0; k < j; k++) {
                const sirio_conv_msg *prior =
                    &messages[i + 1 + (int)k];
                if (!strcmp(result->tool_call_id, prior->tool_call_id)) {
                    worker_copy_err(err, err_len,
                                    "duplicate tool result in session");
                    return false;
                }
            }
        }
        i += (int)message->tool_call_count;
    }
    return true;
}

static bool sirio_session_read(const char *path, bool load_messages,
                               sirio_session_data *data,
                               char *err, size_t err_len) {
    memset(data, 0, sizeof(*data));
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, err_len, "open session: %s", strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        (uint64_t)st.st_size > SIRIO_SESSION_MAX_FILE_BYTES) {
        worker_copy_err(err, err_len, "invalid session file size");
        fclose(fp);
        return false;
    }

    char line[256];
    uint64_t title_bytes = 0, message_count = 0;
    bool ok = sirio_session_read_line(fp, line, sizeof(line), err, err_len);
    if (ok && strcmp(line, SIRIO_SESSION_MAGIC " 2") != 0) {
        worker_copy_err(err, err_len, "unsupported session format");
        ok = false;
    }
    ok = ok && sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
         sirio_session_parse_u64(line, "created_at", UINT64_MAX,
                                 &data->created_at, err, err_len);
    ok = ok && sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
         sirio_session_parse_u64(line, "last_used", UINT64_MAX,
                                 &data->last_used, err, err_len);
    if (ok) {
        char provider[32], model[128], reasoning[16], trailing = '\0';
        ok = sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
             sscanf(line, "selection %31s %127s %15s %c",
                    provider, model, reasoning, &trailing) == 3;
        if (!ok) {
            worker_copy_err(err, err_len, "invalid session model selection");
        } else {
            data->provider = xstrdup(provider);
            data->model = xstrdup(model);
            data->reasoning = xstrdup(reasoning);
        }
    }
    ok = ok && sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
         sirio_session_parse_u64(line, "title", SIRIO_SESSION_MAX_TITLE_BYTES,
                                 &title_bytes, err, err_len);
    ok = ok && sirio_session_read_blob(fp, title_bytes, true, &data->title,
                                       err, err_len);
    ok = ok && sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
         sirio_session_parse_u64(line, "messages", SIRIO_SESSION_MAX_MESSAGES,
                                 &message_count, err, err_len);
    if (ok && load_messages && message_count)
        data->messages = calloc((size_t)message_count, sizeof(*data->messages));
    if (ok && load_messages && message_count && !data->messages) {
        worker_copy_err(err, err_len, "out of memory loading session");
        ok = false;
    }

    uint64_t chars = 0;
    uint64_t total_calls = 0;
    for (uint64_t i = 0; ok && i < message_count; i++) {
        sirio_conv_msg *message = load_messages ? &data->messages[i] : NULL;
        if (load_messages) data->message_count = (int)i + 1;
        sirio_session_message_header header = {0};
        ok = sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
             sirio_session_parse_message(line, &header,
                                         err, err_len);
        if (!ok) break;
        uint64_t message_bytes = header.text_bytes;
        if (header.reasoning_bytes > UINT64_MAX - message_bytes ||
            header.provider_state_bytes >
                UINT64_MAX - message_bytes - header.reasoning_bytes ||
            header.tool_call_id_bytes >
                UINT64_MAX - message_bytes - header.reasoning_bytes -
                    header.provider_state_bytes) {
            worker_copy_err(err, err_len, "session transcript size overflow");
            ok = false;
            break;
        }
        message_bytes += header.reasoning_bytes +
                         header.provider_state_bytes +
                         header.tool_call_id_bytes;
        ok = sirio_session_add_chars(&chars, message_bytes, err, err_len);
        if (!ok || header.tool_call_count >
                   SIRIO_SESSION_MAX_TOOL_CALLS - total_calls) {
            if (ok) worker_copy_err(err, err_len,
                                    "too many tool calls in session");
            ok = false;
            break;
        }
        total_calls += header.tool_call_count;
        if (!(header.flags & 1) && header.reasoning_bytes) {
            worker_copy_err(err, err_len,
                            "reasoning bytes without a presence flag");
            ok = false;
            break;
        }
        if (!(header.flags & 2) && header.tool_call_id_bytes) {
            worker_copy_err(err, err_len,
                            "tool-call id bytes without a presence flag");
            ok = false;
            break;
        }
        if (!(header.flags & 4) && header.provider_state_bytes) {
            worker_copy_err(err, err_len,
                            "provider-state bytes without a presence flag");
            ok = false;
            break;
        }

        char *text = NULL, *reasoning = NULL, *provider_state = NULL;
        char *tool_call_id = NULL;
        ok = sirio_session_read_blob(fp, header.text_bytes, load_messages,
                                     &text, err, err_len) &&
             sirio_session_read_blob(fp, header.reasoning_bytes,
                                     load_messages && (header.flags & 1),
                                     &reasoning, err, err_len);
        if (ok)
            ok = sirio_session_read_blob(fp, header.provider_state_bytes,
                                         load_messages && (header.flags & 4),
                                         &provider_state, err, err_len);
        if (ok)
            ok = sirio_session_read_blob(fp, header.tool_call_id_bytes,
                                         load_messages && (header.flags & 2),
                                         &tool_call_id, err, err_len);
        if (!ok) {
            free(text);
            free(reasoning);
            free(provider_state);
            free(tool_call_id);
            break;
        }
        if (load_messages) {
            message->role = (sirio_role)header.role;
            message->provider = (sirio_provider)header.provider;
            message->text = text;
            message->reasoning = reasoning;
            message->provider_state_json = provider_state;
            message->tool_call_id = tool_call_id;
            message->tool_call_count = (size_t)header.tool_call_count;
            if (message->tool_call_count) {
                message->tool_calls = calloc(message->tool_call_count,
                                             sizeof(*message->tool_calls));
                if (!message->tool_calls) {
                    worker_copy_err(err, err_len,
                                    "out of memory loading session calls");
                    ok = false;
                    break;
                }
            }
        }

        for (uint64_t j = 0; ok && j < header.tool_call_count; j++) {
            uint64_t id_bytes = 0, name_bytes = 0, arguments_bytes = 0;
            ok = sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
                 sirio_session_parse_call(line, &id_bytes, &name_bytes,
                                          &arguments_bytes, err, err_len);
            if (!ok) break;
            uint64_t call_bytes = id_bytes;
            if (name_bytes > UINT64_MAX - call_bytes ||
                arguments_bytes > UINT64_MAX - call_bytes - name_bytes) {
                worker_copy_err(err, err_len,
                                "session tool-call size overflow");
                ok = false;
                break;
            }
            call_bytes += name_bytes + arguments_bytes;
            if (!sirio_session_add_chars(&chars, call_bytes, err, err_len)) {
                ok = false;
                break;
            }
            sirio_tool_call *call = load_messages ?
                &message->tool_calls[j] : NULL;
            char *id = NULL, *name = NULL, *arguments = NULL;
            ok = sirio_session_read_blob(fp, id_bytes, load_messages,
                                         &id, err, err_len) &&
                 sirio_session_read_blob(fp, name_bytes, load_messages,
                                         &name, err, err_len) &&
                 sirio_session_read_blob(fp, arguments_bytes, load_messages,
                                         &arguments, err, err_len);
            if (!ok) {
                free(id);
                free(name);
                free(arguments);
                break;
            }
            if (load_messages) {
                call->id = id;
                call->name = name;
                call->arguments_json = arguments;
            }
        }
    }
    if (ok) {
        ok = sirio_session_read_line(fp, line, sizeof(line), err, err_len) &&
             !strcmp(line, "end");
        if (!ok && (!err || !err[0]))
            worker_copy_err(err, err_len, "invalid session terminator");
    }
    if (ok) {
        int c;
        while ((c = fgetc(fp)) != EOF) {
            if (!isspace((unsigned char)c)) {
                worker_copy_err(err, err_len, "trailing data in session file");
                ok = false;
                break;
            }
        }
        if (ferror(fp)) ok = false;
    }
    if (fclose(fp) != 0 && ok) {
        worker_copy_err(err, err_len, "failed to close session file");
        ok = false;
    }
    if (ok && load_messages)
        ok = sirio_session_validate_messages(data->messages,
                                             data->message_count,
                                             err, err_len);
    if (!ok) {
        sirio_session_data_free(data);
        return false;
    }
    if (!load_messages) data->message_count = (int)message_count;
    data->chars = (int)chars;
    return true;
}

static bool sirio_session_write_blob(FILE *fp, const char *text) {
    size_t len = strlen(text);
    return (!len || fwrite(text, 1, len, fp) == len) && fputc('\n', fp) != EOF;
}

static bool sirio_session_write(const char *path, const char *title,
                                uint64_t created_at, uint64_t last_used,
                                const char *provider, const char *model,
                                const char *reasoning,
                                const sirio_conv_msg *messages, int message_count,
                                char *err, size_t err_len) {
    if (!title || !provider || !provider[0] || !model || !model[0] ||
        !reasoning || !reasoning[0] || strchr(provider, ' ') ||
        strchr(model, ' ') || strchr(reasoning, ' ') || message_count < 0 ||
        (unsigned)message_count > SIRIO_SESSION_MAX_MESSAGES) {
        worker_copy_err(err, err_len, "invalid session state");
        return false;
    }
    size_t title_len = strlen(title);
    if (title_len > SIRIO_SESSION_MAX_TITLE_BYTES) {
        worker_copy_err(err, err_len, "session title is too large");
        return false;
    }
    if (!sirio_session_validate_messages(messages, message_count,
                                         err, err_len))
        return false;
    uint64_t total = 0;
    uint64_t total_calls = 0;
    for (int i = 0; i < message_count; i++) {
        size_t chars = conv_msg_chars(&messages[i]);
        if (chars > SIRIO_SESSION_MAX_BYTES ||
            !sirio_session_add_chars(&total, (uint64_t)chars,
                                     err, err_len))
            return false;
        if (messages[i].tool_call_count >
            SIRIO_SESSION_MAX_TOOL_CALLS - total_calls) {
            worker_copy_err(err, err_len, "too many tool calls in session");
            return false;
        }
        total_calls += messages[i].tool_call_count;
    }

    agent_buf temp = {0};
    agent_buf_puts(&temp, path);
    agent_buf_puts(&temp, ".tmp.XXXXXX");
    int fd = mkstemp(temp.ptr);
    if (fd < 0) {
        snprintf(err, err_len, "create session: %s", strerror(errno));
        agent_buf_release(&temp);
        return false;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        snprintf(err, err_len, "open session stream: %s", strerror(errno));
        close(fd);
        unlink(temp.ptr);
        agent_buf_release(&temp);
        return false;
    }

    bool ok = fprintf(fp, "%s %d\ncreated_at %llu\nlast_used %llu\n"
                      "selection %s %s %s\ntitle %zu\n",
                      SIRIO_SESSION_MAGIC,
                      SIRIO_SESSION_VERSION,
                      (unsigned long long)created_at,
                      (unsigned long long)last_used,
                      provider, model, reasoning, title_len) >= 0 &&
              sirio_session_write_blob(fp, title) &&
              fprintf(fp, "messages %d\n", message_count) >= 0;
    for (int i = 0; ok && i < message_count; i++) {
        const sirio_conv_msg *message = &messages[i];
        size_t text_len = strlen(message->text);
        size_t reasoning_len = message->reasoning ?
                               strlen(message->reasoning) : 0;
        size_t provider_state_len = message->provider_state_json ?
                                    strlen(message->provider_state_json) : 0;
        size_t tool_call_id_len = message->tool_call_id ?
                                  strlen(message->tool_call_id) : 0;
        unsigned flags = (message->reasoning ? 1u : 0u) |
                         (message->tool_call_id ? 2u : 0u) |
                         (message->provider_state_json ? 4u : 0u);
        ok = fprintf(fp, "message %d %d %zu %zu %zu %zu %zu %u\n",
                     message->role, message->provider,
                     text_len, reasoning_len,
                     provider_state_len, tool_call_id_len,
                     message->tool_call_count, flags) >= 0 &&
             sirio_session_write_blob(fp, message->text) &&
             sirio_session_write_blob(fp, message->reasoning ?
                                      message->reasoning : "") &&
             sirio_session_write_blob(fp, message->provider_state_json ?
                                      message->provider_state_json : "") &&
             sirio_session_write_blob(fp, message->tool_call_id ?
                                      message->tool_call_id : "");
        for (size_t j = 0; ok && j < message->tool_call_count; j++) {
            const sirio_tool_call *call = &message->tool_calls[j];
            ok = fprintf(fp, "call %zu %zu %zu\n", strlen(call->id),
                         strlen(call->name),
                         strlen(call->arguments_json)) >= 0 &&
                 sirio_session_write_blob(fp, call->id) &&
                 sirio_session_write_blob(fp, call->name) &&
                 sirio_session_write_blob(fp, call->arguments_json);
        }
    }
    ok = ok && fputs("end\n", fp) != EOF && fflush(fp) == 0 && fsync(fd) == 0;
    if (fclose(fp) != 0) ok = false;
    if (!ok) {
        worker_copy_err(err, err_len, "failed to write session file");
        unlink(temp.ptr);
        agent_buf_release(&temp);
        return false;
    }
    if (rename(temp.ptr, path) != 0) {
        snprintf(err, err_len, "replace session: %s", strerror(errno));
        unlink(temp.ptr);
        agent_buf_release(&temp);
        return false;
    }
    agent_buf_release(&temp);
    return true;
}

static bool sirio_session_validate_identity(const sirio_session_data *data,
                                            const char sha[41],
                                            char *err, size_t err_len) {
    char expected[41];
    agent_session_identity_sha(data->title, data->created_at, expected);
    if (strcmp(expected, sha)) {
        worker_copy_err(err, err_len, "session identity does not match its file name");
        return false;
    }
    return true;
}

typedef struct {
    char sha[41];
    char *title;
    uint64_t created_at;
    uint64_t last_used;
    uint64_t file_bytes;
    int tokens;
} sirio_session_list_item;

static int sirio_session_list_cmp(const void *a, const void *b) {
    const sirio_session_list_item *sa = a, *sb = b;
    uint64_t ta = sa->last_used ? sa->last_used : sa->created_at;
    uint64_t tb = sb->last_used ? sb->last_used : sb->created_at;
    if (ta < tb) return 1;
    if (ta > tb) return -1;
    return strcmp(sa->sha, sb->sha);
}

void agent_worker_list_sessions(agent_worker *w) {
    DIR *dir = opendir(w->cache_dir);
    if (!dir) {
        if (errno == ENOENT) agent_publishf(w, "no saved sessions\n");
        else agent_publishf(w, "cannot list sessions: %s\n", strerror(errno));
        return;
    }
    sirio_session_list_item *items = NULL;
    int len = 0, cap = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char sha[41];
        if (!sirio_session_filename_sha(entry->d_name, sha)) continue;
        char *path = sirio_session_path(w->cache_dir, sha);
        struct stat st;
        sirio_session_data data;
        char read_err[160] = {0};
        bool ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                  sirio_session_read(path, false, &data,
                                     read_err, sizeof(read_err)) &&
                  sirio_session_validate_identity(&data, sha,
                                                  read_err, sizeof(read_err));
        if (!ok) {
            agent_trace(w, "session-list skipped %s: %s", entry->d_name,
                        read_err[0] ? read_err : strerror(errno));
            free(path);
            continue;
        }
        if (len == cap) {
            cap = cap ? cap * 2 : 16;
            items = xrealloc(items, (size_t)cap * sizeof(*items));
        }
        memcpy(items[len].sha, sha, 41);
        items[len].title = data.title;
        data.title = NULL;
        items[len].created_at = data.created_at;
        items[len].last_used = data.last_used;
        if ((uint64_t)st.st_mtime > items[len].last_used)
            items[len].last_used = (uint64_t)st.st_mtime;
        items[len].file_bytes = (uint64_t)st.st_size;
        items[len].tokens = (int)transcript_estimate_chars(data.chars);
        len++;
        sirio_session_data_free(&data);
        free(path);
    }
    closedir(dir);
    if (!len) {
        free(items);
        agent_publishf(w, "no saved sessions\n");
        return;
    }
    qsort(items, (size_t)len, sizeof(*items), sirio_session_list_cmp);
    agent_publishf(w, "saved sessions:\n");
    for (int i = 0; i < len; i++) {
        char age[32];
        agent_format_age(items[i].last_used, age, sizeof(age));
        char *title = agent_session_title_clip(items[i].title, 72);
        agent_publishf(w, "%.8s  %-9s  %d tokens  %llu KiB  %s\n",
                       items[i].sha, age, items[i].tokens,
                       (unsigned long long)((items[i].file_bytes + 1023) / 1024),
                       title);
        free(title);
        free(items[i].title);
    }
    free(items);
}

bool agent_worker_save_session_now(agent_worker *w, char sha_out[41],
                                   int *tokens_out,
                                   char *err, size_t err_len) {
    if (!w || !w->engine || !w->engine->model ||
        w->engine->provider == SIRIO_PROVIDER_NONE) {
        worker_copy_err(err, err_len, "model selection is unavailable");
        return false;
    }
    const char *first_user = NULL;
    for (int i = 0; i < sirio_conv.len; i++) {
        if (sirio_conv.v[i].role == 1) {
            first_user = sirio_conv.v[i].text;
            break;
        }
    }
    if (!first_user) {
        worker_copy_err(err, err_len, "nothing to save: no user turn");
        return false;
    }
    if (!w->cache_dir || !agent_mkdir_p(w->cache_dir)) {
        worker_copy_err(err, err_len, "cannot create Sirio session directory");
        return false;
    }

    bool new_identity = !w->session_sha[0];
    char *title = w->session_title ? xstrdup(w->session_title) :
                  agent_session_title_from_prompt(first_user, 4096);
    uint64_t created_at = w->session_created_at ? w->session_created_at :
                          (uint64_t)time(NULL);
    char sha[41];
    char *path = NULL;
    for (int attempt = 0; ; attempt++) {
        agent_session_identity_sha(title, created_at, sha);
        free(path);
        path = sirio_session_path(w->cache_dir, sha);
        if (!new_identity || access(path, F_OK) != 0) {
            if (!new_identity || errno == ENOENT) break;
            snprintf(err, err_len, "inspect session path: %s", strerror(errno));
            free(path);
            free(title);
            return false;
        }
        if (attempt == 1023 || created_at == UINT64_MAX) {
            worker_copy_err(err, err_len, "could not allocate a unique session identity");
            free(path);
            free(title);
            return false;
        }
        created_at++;
    }

    uint64_t now = (uint64_t)time(NULL);
    if (!sirio_session_write(
                             path, title, created_at, now,
                             sirio_provider_name(w->engine->provider),
                             w->engine->model->name,
                             sirio_reasoning_name(w->engine->reasoning),
                             sirio_conv.v, sirio_conv.len, err, err_len)) {
        free(path);
        free(title);
        return false;
    }
    if (new_identity) {
        memcpy(w->session_sha, sha, 41);
        free(w->session_title);
        w->session_title = title;
        title = NULL;
        w->session_created_at = created_at;
    }
    pthread_mutex_lock(&w->mu);
    w->session_dirty = false;
    pthread_mutex_unlock(&w->mu);
    if (sha_out) memcpy(sha_out, sha, 41);
    if (tokens_out)
        *tokens_out = (int)transcript_estimate_chars(sirio_conv.chars);
    free(path);
    free(title);
    return true;
}

static bool sirio_find_session_in_dir(const char *cache_dir,
                                      const char *prefix,
                                      char sha_out[41], char **path_out,
                                      char *err, size_t err_len) {
    if (sha_out) sha_out[0] = '\0';
    if (path_out) *path_out = NULL;
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    if (!prefix_len || prefix_len > 40) {
        worker_copy_err(err, err_len, "session SHA prefix must contain 1 to 40 hex digits");
        return false;
    }
    char normalized[41];
    for (size_t i = 0; i < prefix_len; i++) {
        unsigned char c = (unsigned char)prefix[i];
        if (!isxdigit(c)) {
            worker_copy_err(err, err_len, "session SHA prefix must contain only hex digits");
            return false;
        }
        normalized[i] = (char)tolower(c);
    }
    normalized[prefix_len] = '\0';

    DIR *dir = opendir(cache_dir);
    if (!dir) {
        if (errno == ENOENT) worker_copy_err(err, err_len, "no matching session");
        else snprintf(err, err_len, "open session directory: %s", strerror(errno));
        return false;
    }
    int matches = 0;
    char match[41] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char sha[41];
        if (!sirio_session_filename_sha(entry->d_name, sha) ||
            strncmp(sha, normalized, prefix_len)) continue;
        char *candidate = sirio_session_path(cache_dir, sha);
        struct stat st;
        bool regular = stat(candidate, &st) == 0 && S_ISREG(st.st_mode);
        free(candidate);
        if (!regular) continue;
        if (++matches == 1) memcpy(match, sha, 41);
    }
    closedir(dir);
    if (!matches) {
        worker_copy_err(err, err_len, "no matching session");
        return false;
    }
    if (matches > 1) {
        worker_copy_err(err, err_len, "ambiguous session SHA prefix");
        return false;
    }
    if (sha_out) memcpy(sha_out, match, 41);
    if (path_out) *path_out = sirio_session_path(cache_dir, match);
    return true;
}

bool agent_worker_find_session(agent_worker *w, const char *prefix,
                               char sha_out[41], char **path_out,
                               char *err, size_t err_len) {
    return sirio_find_session_in_dir(w->cache_dir, prefix, sha_out, path_out,
                                     err, err_len);
}

static void sirio_session_info_set(sirio_session_info *info,
                                   const char sha[41],
                                   const sirio_session_data *data) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    memcpy(info->sha, sha, 41);
    snprintf(info->provider, sizeof(info->provider), "%s",
             data->provider ? data->provider : "");
    snprintf(info->model, sizeof(info->model), "%s",
             data->model ? data->model : "");
    snprintf(info->reasoning, sizeof(info->reasoning), "%s",
             data->reasoning ? data->reasoning : "");
    snprintf(info->title, sizeof(info->title), "%s",
             data->title ? data->title : "");
}

static bool sirio_session_load_prefix(const char *prefix, bool messages,
                                      char sha[41], char **path_out,
                                      sirio_session_data *data,
                                      char *err, size_t err_len) {
    char *dir = sirio_default_session_dir();
    char *path = NULL;
    bool ok = sirio_find_session_in_dir(dir, prefix, sha, &path,
                                        err, err_len) &&
              sirio_session_read(path, messages, data, err, err_len) &&
              sirio_session_validate_identity(data, sha, err, err_len);
    free(dir);
    if (!ok) {
        free(path);
        return false;
    }
    if (path_out) *path_out = path;
    else free(path);
    return true;
}

int sirio_session_inspect(const char *prefix, sirio_session_info *info,
                          char *err, size_t err_len) {
    char sha[41];
    sirio_session_data data;
    if (!sirio_session_load_prefix(prefix, true, sha, NULL, &data,
                                   err, err_len))
        return 1;
    if (!data.message_count || data.messages[0].role != SIRIO_ROLE_SYSTEM) {
        worker_copy_err(err, err_len, "session has no system message");
        sirio_session_data_free(&data);
        return 1;
    }
    sirio_session_info_set(info, sha, &data);
    sirio_session_data_free(&data);
    return 0;
}

int sirio_session_delete(const char *prefix, sirio_session_info *info,
                         char *err, size_t err_len) {
    char sha[41];
    char *path = NULL;
    sirio_session_data data;
    if (!sirio_session_load_prefix(prefix, false, sha, &path, &data,
                                   err, err_len))
        return 1;
    sirio_session_info_set(info, sha, &data);
    sirio_session_data_free(&data);
    if (unlink(path) != 0) {
        snprintf(err, err_len, "delete session: %s", strerror(errno));
        free(path);
        return 1;
    }
    free(path);
    return 0;
}

int sirio_session_strip(const char *prefix, sirio_session_info *info,
                        uint32_t *tokens_out,
                        char *err, size_t err_len) {
    char sha[41];
    char *path = NULL;
    struct stat before;
    sirio_session_data data;
    if (!sirio_session_load_prefix(prefix, true, sha, &path, &data,
                                   err, err_len))
        return 1;
    bool ok = stat(path, &before) == 0;
    if (!ok) snprintf(err, err_len, "stat session: %s", strerror(errno));
    if (ok)
        ok = sirio_session_write(path, data.title, data.created_at,
                                 data.last_used, data.provider, data.model,
                                 data.reasoning, data.messages,
                                 data.message_count, err, err_len);
    if (ok) {
        struct timespec times[2] = {before.st_atim, before.st_mtim};
        if (utimensat(AT_FDCWD, path, times, 0) != 0) {
            snprintf(err, err_len, "restore session timestamp: %s",
                     strerror(errno));
            ok = false;
        }
    }
    if (ok) {
        sirio_session_info_set(info, sha, &data);
        if (tokens_out)
            *tokens_out = (uint32_t)transcript_estimate_chars(data.chars);
    }
    sirio_session_data_free(&data);
    free(path);
    return ok ? 0 : 1;
}

int sirio_sessions_list(FILE *output, char *err, size_t err_len) {
    char *dir_path = sirio_default_session_dir();
    DIR *dir = opendir(dir_path);
    if (!dir) {
        int saved_errno = errno;
        free(dir_path);
        if (saved_errno == ENOENT) {
            fputs("no saved sessions\n", output);
            return 0;
        }
        snprintf(err, err_len, "open session directory: %s",
                 strerror(saved_errno));
        return 1;
    }
    sirio_session_list_item *items = NULL;
    int len = 0, cap = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char sha[41];
        if (!sirio_session_filename_sha(entry->d_name, sha)) continue;
        char *path = sirio_session_path(dir_path, sha);
        struct stat st;
        sirio_session_data data;
        char read_err[160] = {0};
        bool ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                  sirio_session_read(path, false, &data,
                                     read_err, sizeof(read_err)) &&
                  sirio_session_validate_identity(&data, sha,
                                                  read_err, sizeof(read_err));
        free(path);
        if (!ok) continue;
        if (len == cap) {
            cap = cap ? cap * 2 : 16;
            items = xrealloc(items, (size_t)cap * sizeof(*items));
        }
        memcpy(items[len].sha, sha, 41);
        items[len].title = data.title;
        data.title = NULL;
        items[len].created_at = data.created_at;
        items[len].last_used = data.last_used;
        if ((uint64_t)st.st_mtime > items[len].last_used)
            items[len].last_used = (uint64_t)st.st_mtime;
        items[len].file_bytes = (uint64_t)st.st_size;
        items[len].tokens = (int)transcript_estimate_chars(data.chars);
        len++;
        sirio_session_data_free(&data);
    }
    closedir(dir);
    free(dir_path);
    if (!len) {
        free(items);
        fputs("no saved sessions\n", output);
        return 0;
    }
    qsort(items, (size_t)len, sizeof(*items), sirio_session_list_cmp);
    fputs("saved sessions:\n", output);
    for (int i = 0; i < len; i++) {
        char age[32];
        agent_format_age(items[i].last_used, age, sizeof(age));
        char *title = agent_session_title_clip(items[i].title, 72);
        fprintf(output, "%.8s  %-9s  %d tokens  %llu KiB  %s\n",
                items[i].sha, age, items[i].tokens,
                (unsigned long long)((items[i].file_bytes + 1023) / 1024),
                title);
        free(title);
        free(items[i].title);
    }
    free(items);
    return 0;
}

static bool agent_worker_load_session(agent_worker *w, const char *prefix,
                                      int history_turns,
                                      bool restore_selection,
                                      char *err, size_t err_len) {
    if (restore_selection && !worker_is_idle(w)) {
        worker_copy_err(err, err_len, "model is busy");
        return false;
    }
    char sha[41];
    char *path = NULL;
    if (!agent_worker_find_session(w, prefix, sha, &path, err, err_len))
        return false;
    sirio_session_data data;
    bool ok = sirio_session_read(path, true, &data, err, err_len) &&
              sirio_session_validate_identity(&data, sha, err, err_len);
    if (ok && (!data.message_count || data.messages[0].role != 0)) {
        worker_copy_err(err, err_len, "session has no system message");
        ok = false;
    }
    bool selection_changed = data.provider && data.model &&
        (strcmp(data.provider, sirio_provider_name(w->engine->provider)) ||
         strcmp(data.model, w->engine->model->name));
    if (ok && restore_selection && data.model) {
        char selection[192];
        int length = snprintf(selection, sizeof(selection), "%s/%s",
                              data.provider, data.model);
        if (length < 0 || (size_t)length >= sizeof(selection) ||
            !agent_worker_select_model_internal(
                w, selection, data.reasoning, err, err_len))
            ok = false;
    }
    if (!ok) {
        sirio_session_data_free(&data);
        free(path);
        return false;
    }

    sirio_bridge *bridge = w->engine->bridge;
    conv_free();
    sirio_conv.v = data.messages;
    sirio_conv.len = data.message_count;
    sirio_conv.cap = data.message_count;
    sirio_conv.chars = data.chars;
    sirio_conv.bridge = bridge;
    sirio_conv.provider = w->engine->provider;
    data.messages = NULL;
    data.message_count = 0;
    if (!restore_selection && selection_changed) conv_drop_provider_cache();

    agent_worker_clear_session_identity(w);
    memcpy(w->session_sha, sha, 41);
    w->session_title = data.title;
    data.title = NULL;
    w->session_created_at = data.created_at;
    bool user_activity = false;
    for (int i = 0; i < sirio_conv.len; i++)
        if (sirio_conv.v[i].role == 1) user_activity = true;
    pthread_mutex_lock(&w->mu);
    w->user_activity = user_activity;
    w->session_dirty = false;
    pthread_mutex_unlock(&w->mu);
    w->datetime_context_injected = true;
    transcript_sync(w);
    agent_worker_note_system_prompt_seen(w);
    (void)utimensat(AT_FDCWD, path, NULL, 0);
    sirio_session_data_free(&data);
    free(path);
    return agent_worker_show_history(w, history_turns, err, err_len);
}

bool agent_worker_switch_session(agent_worker *w, const char *prefix,
                                 int history_turns,
                                 char *err, size_t err_len) {
    return agent_worker_load_session(w, prefix, history_turns, true,
                                     err, err_len);
}

bool agent_worker_strip_session(agent_worker *w, const char *prefix,
                                char sha_out[41],
                                uint32_t *tokens_out,
                                char *err, size_t err_len) {
    char sha[41];
    char *path = NULL;
    if (!agent_worker_find_session(w, prefix, sha, &path, err, err_len))
        return false;
    struct stat before;
    sirio_session_data data = {0};
    bool ok = stat(path, &before) == 0 &&
              sirio_session_read(path, true, &data, err, err_len) &&
              sirio_session_validate_identity(&data, sha, err, err_len);
    if (!ok && !err[0])
        snprintf(err, err_len, "stat session: %s", strerror(errno));
    if (ok) {
        ok = sirio_session_write(
                                 path, data.title, data.created_at,
                                 data.last_used,
                                 data.provider ? data.provider :
                                     sirio_provider_name(w->engine->provider),
                                 data.model ? data.model : w->engine->model->name,
                                 data.reasoning ? data.reasoning :
                                     sirio_reasoning_name(w->engine->reasoning),
                                 data.messages,
                                 data.message_count, err, err_len);
    }
    if (ok) {
        struct timespec times[2] = {before.st_atim, before.st_mtim};
        if (utimensat(AT_FDCWD, path, times, 0) != 0) {
            snprintf(err, err_len, "restore session timestamp: %s",
                     strerror(errno));
            ok = false;
        }
    }
    if (ok && sha_out) memcpy(sha_out, sha, 41);
    if (ok && tokens_out)
        *tokens_out = (uint32_t)transcript_estimate_chars(data.chars);
    sirio_session_data_free(&data);
    free(path);
    return ok;
}

static agent_worker *agent_completion_worker;

void sirio_worker_set_completion_context(agent_worker *worker) {
    agent_completion_worker = worker;
}

void agent_switch_completion_callback(const char *buf,
                                      linenoiseCompletions *lc) {
    static const char command[] = "/switch";
    if (!agent_completion_worker || strncmp(buf, command, sizeof(command) - 1))
        return;
    const char *p = buf + sizeof(command) - 1;
    if (*p && *p != ' ' && *p != '\t') return;
    while (*p == ' ' || *p == '\t') p++;
    const char *prefix = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) return;
    size_t prefix_len = (size_t)(p - prefix);
    if (prefix_len > 40) return;
    for (size_t i = 0; i < prefix_len; i++)
        if (!isxdigit((unsigned char)prefix[i])) return;

    DIR *dir = opendir(agent_completion_worker->cache_dir);
    if (!dir) return;
    agent_completion_sessions sessions = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char sha[41];
        if (!sirio_session_filename_sha(entry->d_name, sha) ||
            strncasecmp(sha, prefix, prefix_len)) continue;
        char *path = sirio_session_path(agent_completion_worker->cache_dir, sha);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            agent_completion_sessions_push(&sessions, sha,
                                           (uint64_t)st.st_mtime);
        free(path);
    }
    closedir(dir);
    qsort(sessions.v, (size_t)sessions.len, sizeof(sessions.v[0]),
          agent_completion_session_cmp);
    for (int i = 0; i < sessions.len; i++) {
        char completion[64];
        snprintf(completion, sizeof(completion), "/switch %s", sessions.v[i].sha);
        linenoiseAddCompletion(lc, completion);
    }
    free(sessions.v);
}

/* The KV cache limit test has no meaning without the local KV store. The
 * empty stub satisfies the maintained core's adapter seam; nothing calls it,
 * so silence the warning. */
__attribute__((unused))
void test_agent_cache_rejects_impossible_lengths(void) {
}

/* ------------------------------------------------------------------ */
/* Worker lifecycle.                                                    */
/* ------------------------------------------------------------------ */

#ifdef SIRIO_CORE_TEST
enum {
    SIRIO_TEST_WORKER_INIT_FAIL_CACHE = 1,
    SIRIO_TEST_WORKER_INIT_FAIL_MUTEX,
    SIRIO_TEST_WORKER_INIT_FAIL_COND,
    SIRIO_TEST_WORKER_INIT_FAIL_PIPE,
    SIRIO_TEST_WORKER_INIT_FAIL_TRACE,
    SIRIO_TEST_WORKER_INIT_FAIL_THREAD,
};
static bool sirio_test_worker_init_should_fail(int stage);
#define SIRIO_WORKER_INIT_SHOULD_FAIL(stage) \
    sirio_test_worker_init_should_fail(stage)
#else
#define SIRIO_WORKER_INIT_SHOULD_FAIL(stage) false
#endif

/* Bridge cancel hook: lets curl's progress callback observe the Ctrl+C
 * latch (w->interrupt) and abort the in-flight request.  Runs on the worker
 * thread inside sirio_bridge_generate(); w->mu is not held there. */
static int worker_cancel_poll(void *priv) {
    return worker_should_interrupt(priv) ? 1 : 0;
}

static FILE *worker_open_trace(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    if (fchmod(fd, 0600) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    FILE *trace = fdopen(fd, "ab");
    if (!trace) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return trace;
}

static void worker_status_selection_locked(agent_worker *w) {
    if (!w || !w->engine) return;
    snprintf(w->status.provider, sizeof(w->status.provider), "%s",
             sirio_provider_name(w->engine->provider));
    snprintf(w->status.model, sizeof(w->status.model), "%s",
             w->engine->model ? w->engine->model->name : "unknown");
    snprintf(w->status.reasoning, sizeof(w->status.reasoning), "%s",
             sirio_reasoning_name(w->engine->reasoning));
    w->status.ctx_size = w->engine->model &&
                         w->engine->model->context_tokens > 0 ?
                         w->engine->model->context_tokens :
                         agent_worker_effective_ctx_size(w);
}

int agent_worker_init(agent_worker *w, sirio_engine *engine,
                      agent_config *cfg) {
    bool mu_ready = false;
    bool cond_ready = false;
    memset(w, 0, sizeof(*w));
    w->engine = engine;
    w->cfg = cfg;
    sirio_conv.bridge = engine ? engine->bridge : NULL;
    sirio_conv.provider = engine ? engine->provider : SIRIO_PROVIDER_NONE;
    w->wake_fd[0] = -1;
    w->wake_fd[1] = -1;
    w->cache_dir = sirio_default_session_dir();
    bool cache_fail = SIRIO_WORKER_INIT_SHOULD_FAIL(
        SIRIO_TEST_WORKER_INIT_FAIL_CACHE);
    if (cache_fail || !agent_mkdir_p(w->cache_dir)) {
        if (!cache_fail)
            fprintf(stderr,
                    "sirio: failed to create session directory %s: %s\n",
                    w->cache_dir, strerror(errno));
        goto fail;
    }
    if (SIRIO_WORKER_INIT_SHOULD_FAIL(
            SIRIO_TEST_WORKER_INIT_FAIL_MUTEX) ||
        pthread_mutex_init(&w->mu, NULL) != 0)
        goto fail;
    mu_ready = true;
    if (SIRIO_WORKER_INIT_SHOULD_FAIL(
            SIRIO_TEST_WORKER_INIT_FAIL_COND) ||
        pthread_cond_init(&w->cond, NULL) != 0)
        goto fail;
    cond_ready = true;
    w->status.state = AGENT_WORKER_IDLE;
    worker_status_selection_locked(w);
    if (SIRIO_WORKER_INIT_SHOULD_FAIL(
            SIRIO_TEST_WORKER_INIT_FAIL_PIPE) ||
        pipe(w->wake_fd) != 0)
        goto fail;
    int old_flags;
    set_nonblock(w->wake_fd[0], true, &old_flags);
    set_nonblock(w->wake_fd[1], true, &old_flags);

    if (cfg->gen.trace_path && cfg->gen.trace_path[0]) {
        w->trace = worker_open_trace(cfg->gen.trace_path);
        if (!w->trace) {
            fprintf(stderr, "sirio: failed to open trace %s: %s\n",
                    cfg->gen.trace_path, strerror(errno));
            goto fail;
        }
    }
    if (SIRIO_WORKER_INIT_SHOULD_FAIL(SIRIO_TEST_WORKER_INIT_FAIL_TRACE))
        goto fail;
    if (engine) {
        engine->cancel_poll = worker_cancel_poll;
        engine->cancel_poll_private_data = w;
    }
    if (sirio_conv.bridge)
        sirio_bridge_set_cancel_poll(sirio_conv.bridge,
                                     worker_cancel_poll, w);
    if (SIRIO_WORKER_INIT_SHOULD_FAIL(
            SIRIO_TEST_WORKER_INIT_FAIL_THREAD) ||
        pthread_create(&w->thread, NULL, worker_main, w) != 0)
        goto fail;
    return 0;

fail:
    /* The cancel poll points at w: never leave it armed for a worker that
     * failed to start. */
    if (sirio_conv.bridge)
        sirio_bridge_set_cancel_poll(sirio_conv.bridge, NULL, NULL);
    if (engine) {
        engine->cancel_poll = NULL;
        engine->cancel_poll_private_data = NULL;
    }
    if (w->trace) { fclose(w->trace); w->trace = NULL; }
    if (w->wake_fd[0] != -1) close(w->wake_fd[0]);
    if (w->wake_fd[1] != -1) close(w->wake_fd[1]);
    w->wake_fd[0] = w->wake_fd[1] = -1;
    if (cond_ready) pthread_cond_destroy(&w->cond);
    if (mu_ready) pthread_mutex_destroy(&w->mu);
    agent_worker_clear_session_identity(w);
    free(w->queued_user_drain_text);
    w->queued_user_drain_text = NULL;
    free(w->cmd_text);
    w->cmd_text = NULL;
    free(w->out);
    w->out = NULL;
    free(w->cache_dir);
    w->cache_dir = NULL;
    sirio_conv.bridge = NULL;
    return -1;
}

void agent_worker_free(agent_worker *w) {
    worker_stop(w);
    if (w->thread) pthread_join(w->thread, NULL);
    if (sirio_conv.bridge)
        sirio_bridge_set_cancel_poll(sirio_conv.bridge, NULL, NULL);
    if (w->engine) {
        w->engine->cancel_poll = NULL;
        w->engine->cancel_poll_private_data = NULL;
    }
    agent_worker_clear_session_identity(w);
    free(w->queued_user_drain_text);
    w->queued_user_drain_text = NULL;
    if (w->wake_fd[0] >= 0) close(w->wake_fd[0]);
    if (w->wake_fd[1] >= 0) close(w->wake_fd[1]);
    w->wake_fd[0] = w->wake_fd[1] = -1;
    if (w->trace) fclose(w->trace);
    w->trace = NULL;
    free(w->cmd_text);
    w->cmd_text = NULL;
    free(w->out);
    w->out = NULL;
    free(w->cache_dir);
    w->cache_dir = NULL;
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cond);
    conv_free();
}

/* ------------------------------------------------------------------ */
/* Entry point used by sirio.c.                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    SIRIO_OPTIONS_ERROR = -1,
    SIRIO_OPTIONS_OK = 0,
    SIRIO_OPTIONS_HELP = 1,
} sirio_options_result;

static void sirio_config_defaults(agent_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->gen.system = sirio_default_system_extra;
    cfg->gen.ctx_size = SIRIO_MODEL_CONTEXT_TOKENS;
    cfg->gen.n_predict = 0;
    cfg->gen.temperature = 1.0f;
    cfg->gen.top_p = 1.0f;
    cfg->gen.think_mode = SIRIO_THINK_LOW;
}

static const char *sirio_option_arg(int *index, int argc, char **argv,
                                    char *err, size_t err_len) {
    if (*index + 1 < argc) return argv[++(*index)];
    snprintf(err, err_len, "missing value for %s", argv[*index]);
    return NULL;
}

static bool sirio_parse_int_option(const char *text, int minimum, int maximum,
                                   int *value) {
    if (!text || !text[0]) return false;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < minimum || parsed > maximum)
        return false;
    *value = (int)parsed;
    return true;
}

static bool sirio_parse_float_option(const char *text, double minimum,
                                     double maximum, float *value) {
    if (!text || !text[0]) return false;
    errno = 0;
    char *end = NULL;
    double parsed = strtod(text, &end);
    if (errno || !end || *end || !isfinite(parsed) ||
        parsed < minimum || parsed > maximum)
        return false;
    *value = (float)parsed;
    return true;
}

static sirio_options_result sirio_parse_options(agent_config *cfg,
                                                int argc, char **argv,
                                                char *err, size_t err_len) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            return SIRIO_OPTIONS_HELP;
        } else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--prompt")) {
            cfg->gen.prompt = sirio_option_arg(&i, argc, argv, err, err_len);
            if (!cfg->gen.prompt) return SIRIO_OPTIONS_ERROR;
        } else if (!strcmp(argv[i], "--non-interactive")) {
            cfg->non_interactive = true;
        } else if (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--chdir")) {
            cfg->chdir_path = sirio_option_arg(&i, argc, argv, err, err_len);
            if (!cfg->chdir_path) return SIRIO_OPTIONS_ERROR;
        } else if (!strcmp(argv[i], "--raw-prompt")) {
            cfg->gen.raw_prompt = true;
        } else if (!strcmp(argv[i], "--edit-upto")) {
            cfg->edit_upto = true;
        } else if (!strcmp(argv[i], "-sys") ||
                   !strcmp(argv[i], "--system")) {
            cfg->gen.system = sirio_option_arg(&i, argc, argv, err, err_len);
            if (!cfg->gen.system) return SIRIO_OPTIONS_ERROR;
            cfg->gen.system_set = true;
        } else if (!strcmp(argv[i], "--trace")) {
            cfg->gen.trace_path = sirio_option_arg(&i, argc, argv, err, err_len);
            if (!cfg->gen.trace_path) return SIRIO_OPTIONS_ERROR;
        } else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--tokens")) {
            const char *value = sirio_option_arg(&i, argc, argv, err, err_len);
            if (!value) return SIRIO_OPTIONS_ERROR;
            if (!sirio_parse_int_option(value, -1, INT_MAX,
                                        &cfg->gen.n_predict)) {
                snprintf(err, err_len,
                         "%s must be -1 or a nonnegative integer",
                         argv[i - 1]);
                return SIRIO_OPTIONS_ERROR;
            }
            cfg->gen.tokens_set = true;
        } else if (!strcmp(argv[i], "--temp")) {
            const char *value = sirio_option_arg(&i, argc, argv, err, err_len);
            if (!value) return SIRIO_OPTIONS_ERROR;
            if (!sirio_parse_float_option(value, 0.0, 2.0,
                                          &cfg->gen.temperature)) {
                snprintf(err, err_len, "--temp must be between 0 and 2");
                return SIRIO_OPTIONS_ERROR;
            }
            cfg->gen.temperature_set = true;
        } else if (!strcmp(argv[i], "--top-p")) {
            const char *value = sirio_option_arg(&i, argc, argv, err, err_len);
            if (!value) return SIRIO_OPTIONS_ERROR;
            if (!sirio_parse_float_option(value, 0.0, 1.0,
                                          &cfg->gen.top_p)) {
                snprintf(err, err_len, "--top-p must be between 0 and 1");
                return SIRIO_OPTIONS_ERROR;
            }
            cfg->gen.top_p_set = true;
        } else if (!strcmp(argv[i], "--think")) {
            const char *value = sirio_option_arg(&i, argc, argv, err, err_len);
            sirio_reasoning_effort effort;
            if (!value) return SIRIO_OPTIONS_ERROR;
            if (!sirio_reasoning_parse(value, &effort)) {
                snprintf(err, err_len,
                         "--think must be none, low, medium, high, xhigh, or max");
                return SIRIO_OPTIONS_ERROR;
            }
            cfg->gen.think_mode = (sirio_think_mode)effort;
            cfg->gen.reasoning_set = true;
        } else {
            snprintf(err, err_len, "unknown option: %s", argv[i]);
            return SIRIO_OPTIONS_ERROR;
        }
    }

    if (cfg->gen.raw_prompt &&
        (!cfg->non_interactive || !cfg->gen.prompt)) {
        snprintf(err, err_len,
                 "--raw-prompt requires --non-interactive and --prompt");
        return SIRIO_OPTIONS_ERROR;
    }
    if (cfg->non_interactive && !cfg->gen.prompt) {
        snprintf(err, err_len, "--non-interactive requires --prompt");
        return SIRIO_OPTIONS_ERROR;
    }
    return SIRIO_OPTIONS_OK;
}

static bool sirio_generation_from_config(
        const agent_config *cfg, sirio_generation_options *options,
        char *err, size_t err_len) {
    (void)err;
    (void)err_len;
    memset(options, 0, sizeof(*options));
    options->max_tokens = cfg->gen.n_predict;
    options->temperature = -1.0;
    options->top_p = -1.0;
    options->reasoning = (sirio_reasoning_effort)cfg->gen.think_mode;
    options->max_tokens_explicit = cfg->gen.tokens_set &&
                                   cfg->gen.n_predict > 0;
    options->temperature_explicit = cfg->gen.temperature_set;
    options->top_p_explicit = cfg->gen.top_p_set;
    if (cfg->gen.temperature_set)
        options->temperature = cfg->gen.temperature;
    if (cfg->gen.top_p_set) options->top_p = cfg->gen.top_p;
    return true;
}

int sirio_agent_parse_options(agent_config *cfg, int argc, char **argv,
                              char *error, size_t error_len) {
    if (!cfg) return 2;
    sirio_config_defaults(cfg);
    if (error && error_len) error[0] = '\0';
    char local_error[256] = {0};
    char *target = error && error_len ? error : local_error;
    size_t target_len = error && error_len ? error_len : sizeof(local_error);
    sirio_options_result parsed = sirio_parse_options(
        cfg, argc, argv, target, target_len);
    if (parsed == SIRIO_OPTIONS_ERROR) return 2;
    if (parsed == SIRIO_OPTIONS_HELP) return 0;
    sirio_generation_options generation;
    return sirio_generation_from_config(cfg, &generation,
                                        target, target_len) ? 0 : 2;
}

int sirio_agent_run(sirio_engine *engine, const agent_config *parsed_config) {
    if (!parsed_config) return 2;
    agent_config cfg = *parsed_config;
    char err[256] = {0};
    if (cfg.chdir_path && chdir(cfg.chdir_path) != 0) {
        perror("sirio: chdir");
        return 1;
    }
    if (!engine || !engine->model) {
        fputs("sirio: inference provider is not initialized\n", stderr);
        return 1;
    }
    sirio_generation_options generation;
    if (!sirio_generation_from_config(&cfg, &generation,
                                      err, sizeof(err))) {
        fprintf(stderr, "sirio: %s\n", err);
        return 2;
    }
    engine->generation = generation;
    const char *requested_reasoning = cfg.gen.reasoning_set ?
        sirio_reasoning_name((sirio_reasoning_effort)cfg.gen.think_mode) : NULL;
    if (engine->select) {
        int select_status = engine->select(
            engine, engine->model->name, requested_reasoning,
            err, sizeof(err));
        if (select_status != 0) {
            fprintf(stderr, "sirio: %s\n", err[0] ? err :
                    "unable to select inference model");
            return select_status == 1 ? 1 : 2;
        }
    } else {
        if (!cfg.gen.reasoning_set) generation.reasoning = engine->reasoning;
        engine->generation = generation;
        if (!engine->bridge || sirio_bridge_set_generation_options(
                engine->bridge, &generation) != 0) {
        fprintf(stderr, "sirio: %s\n", engine->bridge ?
                sirio_bridge_last_error(engine->bridge) :
                "inference bridge is not initialized");
        return 2;
        }
    }
    cfg.gen.think_mode = (sirio_think_mode)engine->reasoning;
    if (engine->model->context_tokens > 0)
        cfg.gen.ctx_size = engine->model->context_tokens;

    struct sigaction old_int;
    struct sigaction int_action;
    memset(&int_action, 0, sizeof(int_action));
    sigemptyset(&int_action.sa_mask);
    int_action.sa_handler = agent_sigint_handler;
    bool sigint_installed = sigaction(SIGINT, &int_action, &old_int) == 0;

    struct sigaction old_pipe;
    struct sigaction pipe_action;
    memset(&pipe_action, 0, sizeof(pipe_action));
    sigemptyset(&pipe_action.sa_mask);
    pipe_action.sa_handler = SIG_IGN;
    bool sigpipe_installed = sigaction(SIGPIPE, &pipe_action, &old_pipe) == 0;

    sirio_container *container = NULL;
    if (!cfg.gen.raw_prompt) {
        sirio_container_options container_options = {
            .workspace = NULL,
            .context_size = cfg.gen.ctx_size,
            .edit_upto = cfg.edit_upto,
        };
        container = sirio_container_start(
            &container_options, err, sizeof(err));
        if (!container) {
            fprintf(stderr, "sirio: %s\n", err);
            if (sigpipe_installed) sigaction(SIGPIPE, &old_pipe, NULL);
            if (sigint_installed) sigaction(SIGINT, &old_int, NULL);
            return 1;
        }
        cfg.external_tools = container;
    }

    int rc = cfg.non_interactive ?
        run_agent_non_interactive(engine, &cfg) :
        run_agent(engine, &cfg);

    if (container) {
        cfg.external_tools = NULL;
        sirio_container_stop(container);
    }
    if (sigpipe_installed) sigaction(SIGPIPE, &old_pipe, NULL);
    if (sigint_installed) sigaction(SIGINT, &old_int, NULL);
    return rc;
}
