/*
 * Sirio provider implementations.
 *
 * Chat Completions and OpenAI Responses are peer adapters selected through
 * provider/model metadata. The public API is declared in sirio_provider.h
 * because the autonomous worker calls it from a different translation unit.
 *
 * sirio.c owns entry-point policy; this file owns provider-specific transport,
 * authentication, request mapping, and streaming.
 */

#include "sirio_provider.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <poll.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#define DEEPSEEK_ENDPOINT "https://api.deepseek.com/chat/completions"
#define DEEPSEEK_MODEL "deepseek-v4-flash"
#define DEEPSEEK_PRO_MODEL "deepseek-v4-pro"
#define DEEPSEEK_RESPONSE_LIMIT (16U * 1024U * 1024U)

#define OPENCODE_GO_ENDPOINT "https://opencode.ai/zen/go/v1/chat/completions"
#define OPENCODE_GO_MODEL DEEPSEEK_MODEL
#define OPENCODE_GO_GLM_MODEL "glm-5.3"

#define KIMI_ENDPOINT "https://api.kimi.com/coding/v1/chat/completions"
#define KIMI_MODEL "k3"
#define KIMI_256K_MODEL "k3-256k"
#define KIMI_CODING_MODEL "kimi-for-coding"
#define KIMI_CONTEXT_TOKENS 262144

#define OPENAI_AUTH_ISSUER "https://auth.openai.com"
#define OPENAI_AUTHORIZE_ENDPOINT OPENAI_AUTH_ISSUER "/oauth/authorize"
#define OPENAI_TOKEN_ENDPOINT OPENAI_AUTH_ISSUER "/oauth/token"
#define OPENAI_CODEX_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"
#define OPENAI_API_ENDPOINT "https://api.openai.com/v1/responses"
#define OPENAI_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define OPENAI_MODEL "gpt-5.6-luna"
#define OPENAI_MODEL_CONTEXT_TOKENS 272000
#define OPENAI_RESPONSE_LIMIT (32U * 1024U * 1024U)
#define OPENAI_REFRESH_MARGIN 300

typedef struct {
    char *id_token;
    char *access_token;
    char *refresh_token;
    char *account_id;
    time_t access_expires_at;
    time_t refreshed_at;
} openai_auth;

struct sirio_auth_store {
    char *api_keys[SIRIO_PROVIDER_COUNT];
    sirio_auth_method preferred[SIRIO_PROVIDER_COUNT];
    openai_auth openai;
};

static const sirio_provider_info provider_catalog[] = {
    {
        .id = SIRIO_PROVIDER_DEEPSEEK,
        .name = "deepseek",
        .default_model = DEEPSEEK_MODEL,
        .supports_api_key = true,
        .supports_oauth = false,
    },
    {
        .id = SIRIO_PROVIDER_OPENAI,
        .name = "openai",
        .default_model = OPENAI_MODEL,
        .supports_api_key = true,
        .supports_oauth = true,
    },
    {
        .id = SIRIO_PROVIDER_OPENCODE_GO,
        .name = "opencode-go",
        .default_model = OPENCODE_GO_MODEL,
        .supports_api_key = true,
        .supports_oauth = false,
    },
    {
        .id = SIRIO_PROVIDER_KIMI,
        .name = "kimi",
        .default_model = KIMI_MODEL,
        .supports_api_key = true,
        .supports_oauth = false,
    },
};

static const sirio_model_info model_catalog[] = {
    {
        .name = DEEPSEEK_MODEL,
        .default_alias = "flash",
        .provider = SIRIO_PROVIDER_DEEPSEEK,
        .context_tokens = SIRIO_MODEL_CONTEXT_TOKENS,
        .max_output_tokens = SIRIO_MODEL_MAX_OUTPUT_TOKENS,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_NONE) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
    {
        .name = DEEPSEEK_PRO_MODEL,
        .default_alias = "pro",
        .provider = SIRIO_PROVIDER_DEEPSEEK,
        .context_tokens = SIRIO_MODEL_CONTEXT_TOKENS,
        .max_output_tokens = SIRIO_MODEL_MAX_OUTPUT_TOKENS,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_NONE) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
    {
        .name = "gpt-5.6-sol",
        .default_alias = "sol",
        .provider = SIRIO_PROVIDER_OPENAI,
        .context_tokens = OPENAI_MODEL_CONTEXT_TOKENS,
        .max_output_tokens = 128000,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_LOW) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MEDIUM) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_XHIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_LOW,
    },
    {
        .name = "gpt-5.6-terra",
        .default_alias = "terra",
        .provider = SIRIO_PROVIDER_OPENAI,
        .context_tokens = OPENAI_MODEL_CONTEXT_TOKENS,
        .max_output_tokens = 128000,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_LOW) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MEDIUM) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_XHIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_MEDIUM,
    },
    {
        .name = OPENAI_MODEL,
        .default_alias = "luna",
        .provider = SIRIO_PROVIDER_OPENAI,
        .context_tokens = OPENAI_MODEL_CONTEXT_TOKENS,
        .max_output_tokens = 128000,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_LOW) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MEDIUM) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_XHIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_LOW,
    },
    {
        .name = OPENCODE_GO_MODEL,
        .default_alias = "flash",
        .provider = SIRIO_PROVIDER_OPENCODE_GO,
        .context_tokens = SIRIO_MODEL_CONTEXT_TOKENS,
        .max_output_tokens = SIRIO_MODEL_MAX_OUTPUT_TOKENS,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_LOW) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
    {
        .name = DEEPSEEK_PRO_MODEL,
        .default_alias = "pro",
        .provider = SIRIO_PROVIDER_OPENCODE_GO,
        .context_tokens = SIRIO_MODEL_CONTEXT_TOKENS,
        .max_output_tokens = SIRIO_MODEL_MAX_OUTPUT_TOKENS,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
    {
        .name = OPENCODE_GO_GLM_MODEL,
        .default_alias = "glm",
        .provider = SIRIO_PROVIDER_OPENCODE_GO,
        .context_tokens = 1000000,
        .max_output_tokens = 131072,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_LOW) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
    {
        .name = KIMI_MODEL,
        .default_alias = "k3",
        .provider = SIRIO_PROVIDER_KIMI,
        .context_tokens = KIMI_CONTEXT_TOKENS,
        .max_output_tokens = 131072,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_LOW) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
    {
        .name = KIMI_256K_MODEL,
        .default_alias = "k3-256",
        .provider = SIRIO_PROVIDER_KIMI,
        .context_tokens = KIMI_CONTEXT_TOKENS,
        .max_output_tokens = 131072,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_LOW) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH) |
                          SIRIO_REASONING_BIT(SIRIO_REASONING_MAX),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
    {
        .name = KIMI_CODING_MODEL,
        .default_alias = "coding",
        .provider = SIRIO_PROVIDER_KIMI,
        .context_tokens = KIMI_CONTEXT_TOKENS,
        .max_output_tokens = 32768,
        .reasoning_mask = SIRIO_REASONING_BIT(SIRIO_REASONING_HIGH),
        .default_reasoning = SIRIO_REASONING_HIGH,
    },
};

size_t sirio_provider_count(void) {
    return sizeof(provider_catalog) / sizeof(provider_catalog[0]);
}

const sirio_provider_info *sirio_provider_at(size_t index) {
    return index < sirio_provider_count() ? &provider_catalog[index] : NULL;
}

const sirio_provider_info *sirio_provider_get(sirio_provider provider) {
    for (size_t i = 0; i < sirio_provider_count(); i++)
        if (provider_catalog[i].id == provider) return &provider_catalog[i];
    return NULL;
}

const sirio_provider_info *sirio_provider_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sirio_provider_count(); i++)
        if (!strcmp(provider_catalog[i].name, name)) return &provider_catalog[i];
    return NULL;
}

const char *sirio_provider_name(sirio_provider provider) {
    const sirio_provider_info *info = sirio_provider_get(provider);
    return info ? info->name : "unknown";
}

size_t sirio_model_count(void) {
    return sizeof(model_catalog) / sizeof(model_catalog[0]);
}

const sirio_model_info *sirio_model_at(size_t index) {
    return index < sirio_model_count() ? &model_catalog[index] : NULL;
}

const sirio_model_info *sirio_model_find(const char *name) {
    if (!name) return NULL;
    const sirio_model_info *found = NULL;
    for (size_t i = 0; i < sirio_model_count(); i++) {
        if (strcmp(model_catalog[i].name, name)) continue;
        if (found) return NULL;
        found = &model_catalog[i];
    }
    return found;
}

const sirio_model_info *sirio_model_find_for_provider(
        sirio_provider provider, const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sirio_model_count(); i++)
        if (model_catalog[i].provider == provider &&
            !strcmp(model_catalog[i].name, name)) return &model_catalog[i];
    return NULL;
}

const char *sirio_reasoning_name(sirio_reasoning_effort effort) {
    static const char *const names[SIRIO_REASONING_COUNT] = {
        "none", "low", "medium", "high", "xhigh", "max"
    };
    return effort >= SIRIO_REASONING_NONE && effort < SIRIO_REASONING_COUNT ?
           names[effort] : NULL;
}

bool sirio_reasoning_parse(const char *name,
                           sirio_reasoning_effort *effort_out) {
    if (!name) return false;
    for (int effort = SIRIO_REASONING_NONE;
         effort < SIRIO_REASONING_COUNT; effort++) {
        if (strcmp(name, sirio_reasoning_name((sirio_reasoning_effort)effort)))
            continue;
        if (effort_out) *effort_out = (sirio_reasoning_effort)effort;
        return true;
    }
    return false;
}

bool sirio_model_supports_reasoning(const sirio_model_info *model,
                                    sirio_reasoning_effort effort) {
    return model && effort >= SIRIO_REASONING_NONE &&
           effort < SIRIO_REASONING_COUNT &&
           (model->reasoning_mask & SIRIO_REASONING_BIT(effort)) != 0;
}

bool sirio_model_step_reasoning(const sirio_model_info *model,
                                sirio_reasoning_effort current,
                                int direction,
                                sirio_reasoning_effort *next_out) {
    if (!model || !direction || !sirio_model_supports_reasoning(model, current))
        return false;
    int step = direction < 0 ? -1 : 1;
    for (int candidate = (int)current + step;
         candidate >= SIRIO_REASONING_NONE && candidate < SIRIO_REASONING_COUNT;
         candidate += step) {
        if (!sirio_model_supports_reasoning(
                model, (sirio_reasoning_effort)candidate))
            continue;
        if (next_out) *next_out = (sirio_reasoning_effort)candidate;
        return true;
    }
    if (next_out) *next_out = current;
    return false;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} bridge_buffer;

typedef struct {
    const char *cursor;
    const char *end;
    char error[160];
} json_parser;

typedef struct {
    char *content;
    char *reasoning;
    char *error_message;
    char *finish_reason;
    char *request_id;
    char *model;
    sirio_tool_call *calls;
    size_t *call_indices;
    size_t call_count;
    size_t call_capacity;
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    uint64_t total_tokens;
    uint64_t reasoning_tokens;
    uint64_t prompt_cache_hit_tokens;
    uint64_t prompt_cache_miss_tokens;
    int has_message;
    int has_delta;
    int has_usage;
} deepseek_result;

struct sirio_bridge {
    const struct provider_ops *provider;
    sirio_provider provider_id;
    sirio_auth_method auth_method;
    char *api_key;
    char *model;
    char *auth_path;
    openai_auth openai;
    char last_error[256];
    int curl_ready;
    atomic_bool cancelled;
    sirio_bridge_cancel_poll cancel_poll;
    void *cancel_poll_priv;
    sirio_generation_options generation;
};

static int bridge_header_append(struct curl_slist **headers,
                                const char *value);

static char *bridge_strdup(const char *text) {
    if (!text) text = "";
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

static void bridge_set_error(sirio_bridge *bridge, const char *format, ...) {
    if (!bridge) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(bridge->last_error, sizeof(bridge->last_error),
              format, arguments);
    va_end(arguments);
}

static int bridge_prepare_generation_options(
        sirio_bridge *bridge, const sirio_generation_options *options,
        sirio_generation_options *prepared) {
    if (!bridge || !options || !prepared) return -1;
    if (options->reasoning < SIRIO_REASONING_NONE ||
        options->reasoning >= SIRIO_REASONING_COUNT) {
        bridge_set_error(bridge, "invalid reasoning mode");
        return -1;
    }
    const sirio_provider_info *provider = sirio_provider_get(
        bridge->provider_id);
    const char *model_name = bridge->model ? bridge->model :
                             provider ? provider->default_model : NULL;
    const sirio_model_info *model = sirio_model_find_for_provider(
        bridge->provider_id, model_name);
    if (!model) {
        bridge_set_error(bridge, "model %s is not available from %s",
                         model_name ? model_name : "(none)",
                         sirio_provider_name(bridge->provider_id));
        return -1;
    }
    if (!sirio_model_supports_reasoning(model, options->reasoning)) {
        bridge_set_error(bridge, "%s does not support reasoning effort %s",
                         model->name,
                         sirio_reasoning_name(options->reasoning));
        return -1;
    }
    if (model->max_output_tokens > 0 &&
        options->max_tokens > model->max_output_tokens) {
        bridge_set_error(bridge,
                         "max_tokens exceeds %s's %d-token output limit",
                         model->name, model->max_output_tokens);
        return -1;
    }
    if (!isfinite(options->temperature) || options->temperature > 2.0) {
        bridge_set_error(bridge, "temperature must be between 0 and 2, or negative to omit");
        return -1;
    }
    if (!isfinite(options->top_p) || options->top_p > 1.0) {
        bridge_set_error(bridge, "top-p must be between 0 and 1, or negative to omit");
        return -1;
    }
    if (bridge->provider_id == SIRIO_PROVIDER_OPENAI &&
        bridge->auth_method == SIRIO_AUTH_OAUTH &&
        options->max_tokens_explicit) {
        bridge_set_error(bridge,
                         "--tokens is unsupported by OpenAI OAuth; omit it or use API-key authentication");
        return -1;
    }
    if (bridge->provider_id == SIRIO_PROVIDER_OPENAI &&
        bridge->auth_method == SIRIO_AUTH_OAUTH &&
        (options->temperature_explicit || options->top_p_explicit)) {
        bridge_set_error(bridge,
                         "--temp and --top-p are unsupported by OpenAI OAuth");
        return -1;
    }
    if (options->reasoning != SIRIO_REASONING_NONE &&
        (options->temperature >= 0.0 || options->top_p >= 0.0)) {
        bridge_set_error(bridge,
                         "temperature and top-p are unsupported by %s in reasoning mode",
                         model->name);
        return -1;
    }
    *prepared = *options;
    if (prepared->max_tokens < 0) prepared->max_tokens = 0;
    if (prepared->temperature < 0.0) prepared->temperature = -1.0;
    if (prepared->top_p < 0.0) prepared->top_p = -1.0;
    return 0;
}

static int buffer_reserve(bridge_buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1) return -1;
    size_t required = buffer->length + extra + 1;
    if (required <= buffer->capacity) return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 512;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *replacement = realloc(buffer->data, capacity);
    if (!replacement) return -1;
    buffer->data = replacement;
    buffer->capacity = capacity;
    return 0;
}

static int buffer_append(bridge_buffer *buffer,
                         const char *data, size_t length) {
    if (!length) return 0;
    if (buffer_reserve(buffer, length) != 0) return -1;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int buffer_puts(bridge_buffer *buffer, const char *text) {
    return buffer_append(buffer, text, strlen(text));
}

static int buffer_putc(bridge_buffer *buffer, char byte) {
    return buffer_append(buffer, &byte, 1);
}

static char *buffer_release(bridge_buffer *buffer) {
    if (!buffer->data) return bridge_strdup("");
    char *data = buffer->data;
    memset(buffer, 0, sizeof(*buffer));
    return data;
}

static void buffer_free(bridge_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int json_append_string(bridge_buffer *buffer, const char *text) {
    if (!text) text = "";
    if (buffer_putc(buffer, '"') != 0) return -1;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor; cursor++) {
        char escaped[7];
        const char *replacement = NULL;
        switch (*cursor) {
        case '"': replacement = "\\\""; break;
        case '\\': replacement = "\\\\"; break;
        case '\b': replacement = "\\b"; break;
        case '\f': replacement = "\\f"; break;
        case '\n': replacement = "\\n"; break;
        case '\r': replacement = "\\r"; break;
        case '\t': replacement = "\\t"; break;
        default:
            if (*cursor < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                replacement = escaped;
            }
            break;
        }
        if (replacement) {
            if (buffer_puts(buffer, replacement) != 0) return -1;
        } else if (buffer_append(buffer, (const char *)cursor, 1) != 0) {
            return -1;
        }
    }
    return buffer_putc(buffer, '"');
}

static int append_tool_call_json(bridge_buffer *request,
                                 const sirio_tool_call *call) {
    if (buffer_puts(request, "{\"id\":") != 0 ||
        json_append_string(request, call->id) != 0 ||
        buffer_puts(request, ",\"type\":\"function\",\"function\":{\"name\":") != 0 ||
        json_append_string(request, call->name) != 0 ||
        buffer_puts(request, ",\"arguments\":") != 0 ||
        json_append_string(request, call->arguments_json) != 0 ||
        buffer_puts(request, "}}") != 0)
        return -1;
    return 0;
}

static bool bridge_utf8_valid(const char *text);
static int json_object_text_is_valid(const char *text);
static int json_array_text_is_valid(const char *text);

static int append_message_json(bridge_buffer *request,
                               const sirio_message *message,
                               sirio_provider provider) {
    if (buffer_puts(request, "{\"role\":") != 0 ||
        json_append_string(request, sirio_role_name(message->role)) != 0 ||
        buffer_puts(request, ",\"content\":") != 0)
        return -1;

    if (message->role == SIRIO_ROLE_ASSISTANT &&
        message->tool_call_count && (!message->content ||
                                     !message->content[0])) {
        if (buffer_puts(request, "null") != 0) return -1;
    } else if (json_append_string(request, message->content) != 0) {
        return -1;
    }

    if (message->role == SIRIO_ROLE_ASSISTANT && message->reasoning &&
        (message->provider == SIRIO_PROVIDER_NONE ||
         message->provider == provider)) {
        if (buffer_puts(request, ",\"reasoning_content\":") != 0 ||
            json_append_string(request, message->reasoning) != 0)
            return -1;
    }

    if (message->role == SIRIO_ROLE_TOOL) {
        if (buffer_puts(request, ",\"tool_call_id\":") != 0 ||
            json_append_string(request, message->tool_call_id) != 0)
            return -1;
    }

    if (message->role == SIRIO_ROLE_ASSISTANT &&
        message->tool_call_count) {
        if (buffer_puts(request, ",\"tool_calls\":[") != 0) return -1;
        for (size_t index = 0; index < message->tool_call_count; index++) {
            if (index && buffer_putc(request, ',') != 0) return -1;
            if (append_tool_call_json(request,
                                      &message->tool_calls[index]) != 0)
                return -1;
        }
        if (buffer_putc(request, ']') != 0) return -1;
    }
    return buffer_putc(request, '}');
}

static int message_is_valid(const sirio_message *message) {
    if (!message || message->role < SIRIO_ROLE_SYSTEM ||
        message->role > SIRIO_ROLE_TOOL ||
        message->provider < SIRIO_PROVIDER_NONE ||
        message->provider >= SIRIO_PROVIDER_COUNT)
        return 0;
    if (message->role != SIRIO_ROLE_ASSISTANT &&
        (message->reasoning || message->provider_state_json ||
         message->tool_calls ||
         message->tool_call_count))
        return 0;
    if (!message->content &&
        !(message->role == SIRIO_ROLE_ASSISTANT &&
          message->tool_call_count))
        return 0;
    if ((message->content && !bridge_utf8_valid(message->content)) ||
        (message->reasoning &&
         !bridge_utf8_valid(message->reasoning)) ||
        (message->provider_state_json &&
         (!bridge_utf8_valid(message->provider_state_json) ||
          !json_array_text_is_valid(message->provider_state_json))) ||
        (message->tool_call_id &&
         !bridge_utf8_valid(message->tool_call_id)))
        return 0;
    if (message->role != SIRIO_ROLE_TOOL && message->tool_call_id)
        return 0;
    if (message->role == SIRIO_ROLE_TOOL &&
        (!message->tool_call_id || !message->tool_call_id[0]))
        return 0;
    if (message->role == SIRIO_ROLE_ASSISTANT) {
        if ((message->tool_call_count != 0) != (message->tool_calls != NULL))
            return 0;
        for (size_t i = 0; i < message->tool_call_count; i++) {
            const sirio_tool_call *call = &message->tool_calls[i];
            if (!call->id || !call->id[0] || !call->name || !call->name[0] ||
                !call->arguments_json || !bridge_utf8_valid(call->id) ||
                !bridge_utf8_valid(call->name) ||
                !bridge_utf8_valid(call->arguments_json))
                return 0;
            for (size_t j = 0; j < i; j++) {
                if (!strcmp(call->id, message->tool_calls[j].id)) return 0;
            }
        }
    }
    return 1;
}

static int messages_are_valid(const sirio_message *messages,
                              size_t message_count) {
    if (!messages || !message_count) return 0;
    for (size_t i = 0; i < message_count; i++) {
        const sirio_message *message = &messages[i];
        if (!message_is_valid(message)) return 0;
        if (message->role == SIRIO_ROLE_TOOL) return 0;
        if (message->role != SIRIO_ROLE_ASSISTANT ||
            !message->tool_call_count)
            continue;
        if (message->tool_call_count > message_count - i - 1) return 0;
        for (size_t result_index = 0;
             result_index < message->tool_call_count; result_index++) {
            const sirio_message *result = &messages[i + 1 + result_index];
            if (!message_is_valid(result) ||
                result->role != SIRIO_ROLE_TOOL)
                return 0;
            size_t matches = 0;
            for (size_t call_index = 0;
                 call_index < message->tool_call_count; call_index++) {
                if (!strcmp(result->tool_call_id,
                            message->tool_calls[call_index].id))
                    matches++;
            }
            if (matches != 1) return 0;
            for (size_t prior = 0; prior < result_index; prior++) {
                if (!strcmp(result->tool_call_id,
                            messages[i + 1 + prior].tool_call_id))
                    return 0;
            }
        }
        i += message->tool_call_count;
    }
    return 1;
}

static int tools_are_valid(const sirio_tool *tools, size_t tool_count) {
    if ((tool_count != 0) != (tools != NULL)) return 0;
    for (size_t i = 0; i < tool_count; i++) {
        if (!tools[i].name || !tools[i].name[0] ||
            !tools[i].description || !tools[i].input_schema_json ||
            !bridge_utf8_valid(tools[i].name) ||
            !bridge_utf8_valid(tools[i].description) ||
            !bridge_utf8_valid(tools[i].input_schema_json) ||
            !json_object_text_is_valid(tools[i].input_schema_json))
            return 0;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(tools[i].name, tools[j].name)) return 0;
    }
    return 1;
}

static int append_tool_json(bridge_buffer *request,
                            const sirio_tool *tool) {
    if (buffer_puts(request,
                    "{\"type\":\"function\",\"function\":{\"name\":") != 0 ||
        json_append_string(request, tool->name) != 0 ||
        buffer_puts(request, ",\"description\":") != 0 ||
        json_append_string(request, tool->description) != 0 ||
        buffer_puts(request, ",\"parameters\":") != 0 ||
        buffer_puts(request, tool->input_schema_json ?
                    tool->input_schema_json : "{}") != 0 ||
        buffer_puts(request, "}}") != 0)
        return -1;
    return 0;
}

static int append_json_double(bridge_buffer *request, const char *name,
                              double value) {
    char number[64];
    int length = snprintf(number, sizeof(number), "%.17g", value);
    if (length <= 0 || (size_t)length >= sizeof(number)) return -1;
    if (buffer_puts(request, ",\"") != 0 ||
        buffer_puts(request, name) != 0 ||
        buffer_puts(request, "\":") != 0 ||
        buffer_puts(request, number) != 0)
        return -1;
    return 0;
}

static int append_json_int(bridge_buffer *request, const char *name,
                           int value) {
    char number[32];
    int length = snprintf(number, sizeof(number), "%d", value);
    if (length <= 0 || (size_t)length >= sizeof(number)) return -1;
    if (buffer_puts(request, ",\"") != 0 ||
        buffer_puts(request, name) != 0 ||
        buffer_puts(request, "\":") != 0 ||
        buffer_puts(request, number) != 0)
        return -1;
    return 0;
}

static int build_request_for_model(bridge_buffer *request,
                         sirio_provider provider,
                         const char *model,
                         const sirio_generation_options *generation,
                         const sirio_message *messages,
                         size_t message_count,
                         const sirio_tool *tools,
                         size_t tool_count) {
    if (!model || !model[0] || !generation ||
        !messages_are_valid(messages, message_count) ||
        !tools_are_valid(tools, tool_count))
        return -1;
    if (buffer_puts(request, "{\"model\":") != 0 ||
        json_append_string(request, model) != 0 ||
        buffer_puts(request, ",\"messages\":[") != 0)
        return -1;
    for (size_t index = 0; index < message_count; index++) {
        if (index && buffer_putc(request, ',') != 0) return -1;
        if (append_message_json(request, &messages[index], provider) != 0)
            return -1;
    }
    if (buffer_putc(request, ']') != 0) return -1;

    if (tool_count) {
        if (buffer_puts(request, ",\"tools\":[") != 0) return -1;
        for (size_t index = 0; index < tool_count; index++) {
            if (index && buffer_putc(request, ',') != 0) return -1;
            if (append_tool_json(request, &tools[index]) != 0) return -1;
        }
        if (buffer_putc(request, ']') != 0) return -1;
        if (generation->reasoning == SIRIO_REASONING_NONE &&
            buffer_puts(request, ",\"tool_choice\":\"auto\"") != 0)
            return -1;
    }

    if (generation->max_tokens > 0 &&
        append_json_int(request, "max_tokens", generation->max_tokens) != 0)
        return -1;

    if (generation->reasoning == SIRIO_REASONING_NONE) {
        if (buffer_puts(request,
                        ",\"thinking\":{\"type\":\"disabled\"}") != 0)
            return -1;
        if (generation->temperature >= 0.0 &&
            append_json_double(request, "temperature",
                               generation->temperature) != 0)
            return -1;
        if (generation->top_p >= 0.0 &&
            append_json_double(request, "top_p", generation->top_p) != 0)
            return -1;
    } else {
        if (buffer_puts(request,
                        ",\"thinking\":{\"type\":\"enabled\"}") != 0)
            return -1;
        if (!(provider == SIRIO_PROVIDER_KIMI &&
              !strcmp(model, KIMI_CODING_MODEL))) {
            const char *effort = sirio_reasoning_name(generation->reasoning);
            if (!effort ||
                buffer_puts(request, ",\"reasoning_effort\":") != 0 ||
                json_append_string(request, effort) != 0)
                return -1;
        }
    }
    return buffer_puts(request,
        ",\"stream\":true,\"stream_options\":{\"include_usage\":true}}");
}

/* Kept for direct adapter contract tests. */
#ifdef SIRIO_NO_MAIN
static int __attribute__((unused)) build_request(bridge_buffer *request,
                         const sirio_generation_options *generation,
                         const sirio_message *messages,
                         size_t message_count,
                         const sirio_tool *tools,
                         size_t tool_count) {
    return build_request_for_model(request, SIRIO_PROVIDER_DEEPSEEK,
                                   DEEPSEEK_MODEL, generation,
                                   messages, message_count, tools, tool_count);
}
#endif

static int http_progress(void *private_data,
                         curl_off_t download_total,
                         curl_off_t download_now,
                         curl_off_t upload_total,
                         curl_off_t upload_now) {
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    sirio_bridge *bridge = private_data;
    if (atomic_load_explicit(&bridge->cancelled, memory_order_relaxed)) return 1;
    if (bridge->cancel_poll && bridge->cancel_poll(bridge->cancel_poll_priv))
        return 1;
    return 0;
}

static void parser_skip_space(json_parser *parser) {
    while (parser->cursor < parser->end &&
           isspace((unsigned char)*parser->cursor))
        parser->cursor++;
}

static int parser_fail(json_parser *parser, const char *message) {
    if (!parser->error[0])
        snprintf(parser->error, sizeof(parser->error), "%s", message);
    return -1;
}

static int parser_expect(json_parser *parser, char expected) {
    parser_skip_space(parser);
    if (parser->cursor >= parser->end || *parser->cursor != expected)
        return parser_fail(parser, "unexpected JSON token");
    parser->cursor++;
    return 0;
}

static int parser_match(json_parser *parser, const char *literal) {
    parser_skip_space(parser);
    size_t length = strlen(literal);
    if ((size_t)(parser->end - parser->cursor) < length ||
        memcmp(parser->cursor, literal, length) != 0)
        return 0;
    parser->cursor += length;
    return 1;
}

static int hex_value(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static int parser_hex4(json_parser *parser, uint32_t *value) {
    if ((size_t)(parser->end - parser->cursor) < 4)
        return parser_fail(parser, "truncated JSON unicode escape");
    uint32_t result = 0;
    for (int index = 0; index < 4; index++) {
        int digit = hex_value(parser->cursor[index]);
        if (digit < 0)
            return parser_fail(parser, "invalid JSON unicode escape");
        result = result * 16 + (uint32_t)digit;
    }
    parser->cursor += 4;
    *value = result;
    return 0;
}

static int buffer_append_codepoint(bridge_buffer *buffer, uint32_t codepoint) {
    char encoded[4];
    size_t length;
    if (codepoint <= 0x7f) {
        encoded[0] = (char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7ff) {
        encoded[0] = (char)(0xc0 | (codepoint >> 6));
        encoded[1] = (char)(0x80 | (codepoint & 0x3f));
        length = 2;
    } else if (codepoint <= 0xffff) {
        encoded[0] = (char)(0xe0 | (codepoint >> 12));
        encoded[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        encoded[2] = (char)(0x80 | (codepoint & 0x3f));
        length = 3;
    } else if (codepoint <= 0x10ffff) {
        encoded[0] = (char)(0xf0 | (codepoint >> 18));
        encoded[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        encoded[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        encoded[3] = (char)(0x80 | (codepoint & 0x3f));
        length = 4;
    } else {
        return -1;
    }
    return buffer_append(buffer, encoded, length);
}

static int parser_string(json_parser *parser, char **result) {
    parser_skip_space(parser);
    if (parser->cursor >= parser->end || *parser->cursor != '"')
        return parser_fail(parser, "expected JSON string");
    parser->cursor++;
    bridge_buffer decoded = {0};
    while (parser->cursor < parser->end) {
        unsigned char byte = (unsigned char)*parser->cursor++;
        if (byte == '"') {
            *result = buffer_release(&decoded);
            if (!*result) return parser_fail(parser, "out of memory");
            return 0;
        }
        if (byte < 0x20) {
            buffer_free(&decoded);
            return parser_fail(parser, "control byte in JSON string");
        }
        if (byte != '\\') {
            if (buffer_append(&decoded, (const char *)&byte, 1) != 0) {
                buffer_free(&decoded);
                return parser_fail(parser, "out of memory");
            }
            continue;
        }
        if (parser->cursor >= parser->end) {
            buffer_free(&decoded);
            return parser_fail(parser, "truncated JSON escape");
        }
        char escape = *parser->cursor++;
        const char *replacement = NULL;
        char replaced;
        switch (escape) {
        case '"': replaced = '"'; replacement = &replaced; break;
        case '\\': replaced = '\\'; replacement = &replaced; break;
        case '/': replaced = '/'; replacement = &replaced; break;
        case 'b': replaced = '\b'; replacement = &replaced; break;
        case 'f': replaced = '\f'; replacement = &replaced; break;
        case 'n': replaced = '\n'; replacement = &replaced; break;
        case 'r': replaced = '\r'; replacement = &replaced; break;
        case 't': replaced = '\t'; replacement = &replaced; break;
        case 'u': {
            uint32_t codepoint;
            if (parser_hex4(parser, &codepoint) != 0) {
                buffer_free(&decoded);
                return -1;
            }
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if ((size_t)(parser->end - parser->cursor) < 6 ||
                    parser->cursor[0] != '\\' ||
                    parser->cursor[1] != 'u') {
                    buffer_free(&decoded);
                    return parser_fail(parser, "missing low surrogate");
                }
                parser->cursor += 2;
                uint32_t low;
                if (parser_hex4(parser, &low) != 0) {
                    buffer_free(&decoded);
                    return -1;
                }
                if (low < 0xdc00 || low > 0xdfff) {
                    buffer_free(&decoded);
                    return parser_fail(parser, "invalid low surrogate");
                }
                codepoint = 0x10000 +
                    ((codepoint - 0xd800) << 10) + (low - 0xdc00);
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                buffer_free(&decoded);
                return parser_fail(parser, "unexpected low surrogate");
            }
            if (codepoint == 0) {
                buffer_free(&decoded);
                return parser_fail(parser, "NUL in JSON string");
            }
            if (buffer_append_codepoint(&decoded, codepoint) != 0) {
                buffer_free(&decoded);
                return parser_fail(parser, "out of memory");
            }
            continue;
        }
        default:
            buffer_free(&decoded);
            return parser_fail(parser, "invalid JSON escape");
        }
        if (buffer_append(&decoded, replacement, 1) != 0) {
            buffer_free(&decoded);
            return parser_fail(parser, "out of memory");
        }
    }
    buffer_free(&decoded);
    return parser_fail(parser, "unterminated JSON string");
}

static int parser_skip_value(json_parser *parser, int depth);

static int parser_skip_object(json_parser *parser, int depth) {
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0) return -1;
        free(key);
        if (parser_expect(parser, ':') != 0 ||
            parser_skip_value(parser, depth + 1) != 0)
            return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parser_skip_array(json_parser *parser, int depth) {
    if (parser_expect(parser, '[') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        if (parser_skip_value(parser, depth + 1) != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == ']') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parser_skip_number(json_parser *parser) {
    parser_skip_space(parser);
    const char *start = parser->cursor;
    if (parser->cursor < parser->end && *parser->cursor == '-')
        parser->cursor++;
    if (parser->cursor < parser->end && *parser->cursor == '0') {
        parser->cursor++;
    } else {
        const char *digits = parser->cursor;
        while (parser->cursor < parser->end &&
               isdigit((unsigned char)*parser->cursor))
            parser->cursor++;
        if (parser->cursor == digits)
            return parser_fail(parser, "invalid JSON number");
    }
    if (parser->cursor < parser->end && *parser->cursor == '.') {
        parser->cursor++;
        const char *digits = parser->cursor;
        while (parser->cursor < parser->end &&
               isdigit((unsigned char)*parser->cursor))
            parser->cursor++;
        if (parser->cursor == digits)
            return parser_fail(parser, "invalid JSON number fraction");
    }
    if (parser->cursor < parser->end &&
        (*parser->cursor == 'e' || *parser->cursor == 'E')) {
        parser->cursor++;
        if (parser->cursor < parser->end &&
            (*parser->cursor == '+' || *parser->cursor == '-'))
            parser->cursor++;
        const char *digits = parser->cursor;
        while (parser->cursor < parser->end &&
               isdigit((unsigned char)*parser->cursor))
            parser->cursor++;
        if (parser->cursor == digits)
            return parser_fail(parser, "invalid JSON number exponent");
    }
    if (parser->cursor == start)
        return parser_fail(parser, "invalid JSON number");
    return 0;
}

static int parser_u64(json_parser *parser, uint64_t *value) {
    parser_skip_space(parser);
    if (parser->cursor >= parser->end ||
        !isdigit((unsigned char)*parser->cursor))
        return parser_fail(parser, "expected nonnegative integer");
    uint64_t parsed = 0;
    while (parser->cursor < parser->end &&
           isdigit((unsigned char)*parser->cursor)) {
        unsigned digit = (unsigned)(*parser->cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10)
            return parser_fail(parser, "JSON integer overflow");
        parsed = parsed * 10 + digit;
        parser->cursor++;
    }
    *value = parsed;
    return 0;
}

static int parser_skip_value(json_parser *parser, int depth) {
    if (depth > 64) return parser_fail(parser, "JSON nesting too deep");
    parser_skip_space(parser);
    if (parser->cursor >= parser->end)
        return parser_fail(parser, "truncated JSON value");
    if (*parser->cursor == '{') return parser_skip_object(parser, depth);
    if (*parser->cursor == '[') return parser_skip_array(parser, depth);
    if (*parser->cursor == '"') {
        char *discarded = NULL;
        int result = parser_string(parser, &discarded);
        free(discarded);
        return result;
    }
    if (parser_match(parser, "true") ||
        parser_match(parser, "false") ||
        parser_match(parser, "null"))
        return 0;
    return parser_skip_number(parser);
}

static int json_object_text_is_valid(const char *text) {
    if (!text) return 0;
    json_parser parser = {
        .cursor = text,
        .end = text + strlen(text),
    };
    parser_skip_space(&parser);
    if (parser.cursor >= parser.end || *parser.cursor != '{' ||
        parser_skip_object(&parser, 0) != 0)
        return 0;
    parser_skip_space(&parser);
    return parser.cursor == parser.end;
}

static int json_array_text_is_valid(const char *text) {
    if (!text) return 0;
    json_parser parser = {
        .cursor = text,
        .end = text + strlen(text),
    };
    parser_skip_space(&parser);
    if (parser.cursor >= parser.end || *parser.cursor != '[' ||
        parser_skip_array(&parser, 0) != 0)
        return 0;
    parser_skip_space(&parser);
    return parser.cursor == parser.end;
}

static int parser_nullable_string(json_parser *parser, char **result) {
    parser_skip_space(parser);
    if (parser_match(parser, "null")) {
        *result = NULL;
        return 0;
    }
    return parser_string(parser, result);
}

static void result_free(deepseek_result *result) {
    free(result->content);
    free(result->reasoning);
    free(result->error_message);
    free(result->finish_reason);
    free(result->request_id);
    free(result->model);
    for (size_t index = 0; index < result->call_count; index++) {
        free(result->calls[index].id);
        free(result->calls[index].name);
        free(result->calls[index].arguments_json);
    }
    free(result->calls);
    free(result->call_indices);
    memset(result, 0, sizeof(*result));
}

static int result_add_call_at(deepseek_result *result,
                              sirio_tool_call *call, size_t call_index) {
    if (result->call_count == result->call_capacity) {
        size_t capacity = result->call_capacity ?
            result->call_capacity * 2 : 4;
        if (capacity > SIZE_MAX / sizeof(*result->calls) ||
            capacity > SIZE_MAX / sizeof(*result->call_indices))
            return -1;
        sirio_tool_call *calls = calloc(capacity, sizeof(*calls));
        size_t *indices = malloc(capacity * sizeof(*indices));
        if (!calls || !indices) {
            free(calls);
            free(indices);
            return -1;
        }
        if (result->call_count) {
            memcpy(calls, result->calls,
                   result->call_count * sizeof(*calls));
            memcpy(indices, result->call_indices,
                   result->call_count * sizeof(*indices));
        }
        free(result->calls);
        free(result->call_indices);
        result->calls = calls;
        result->call_indices = indices;
        result->call_capacity = capacity;
    }
    result->calls[result->call_count++] = *call;
    result->call_indices[result->call_count - 1] = call_index;
    memset(call, 0, sizeof(*call));
    return 0;
}

static int result_add_call(deepseek_result *result,
                           sirio_tool_call *call) {
    return result_add_call_at(result, call, result->call_count);
}

static int parse_function(json_parser *parser, sirio_tool_call *call) {
    parser_skip_space(parser);
    if (parser_match(parser, "null")) return 0;
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status;
        if (!strcmp(key, "name")) {
            free(call->name);
            status = parser_nullable_string(parser, &call->name);
        } else if (!strcmp(key, "arguments")) {
            free(call->arguments_json);
            status = parser_nullable_string(parser, &call->arguments_json);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_tool_call(json_parser *parser, deepseek_result *result) {
    sirio_tool_call call = {0};
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return parser_fail(parser, "empty tool call");
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            goto fail;
        }
        int status;
        if (!strcmp(key, "id")) {
            free(call.id);
            status = parser_string(parser, &call.id);
        } else if (!strcmp(key, "function")) {
            status = parse_function(parser, &call);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) goto fail;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            break;
        }
        if (parser_expect(parser, ',') != 0) goto fail;
    }
    if (!call.id || !call.name || !call.arguments_json) {
        parser_fail(parser, "incomplete tool call");
        goto fail;
    }
    if (result_add_call(result, &call) != 0) {
        parser_fail(parser, "out of memory");
        goto fail;
    }
    return 0;

fail:
    free(call.id);
    free(call.name);
    free(call.arguments_json);
    return -1;
}

static int parse_tool_calls(json_parser *parser, deepseek_result *result) {
    parser_skip_space(parser);
    if (parser_match(parser, "null")) return 0;
    if (parser_expect(parser, '[') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        if (parse_tool_call(parser, result) != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == ']') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_tool_call_delta(json_parser *parser,
                                 deepseek_result *result) {
    sirio_tool_call call = {0};
    uint64_t call_index = 0;
    bool has_index = false;
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return parser_fail(parser, "empty tool-call delta");
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            goto fail;
        }
        int status;
        if (!strcmp(key, "index")) {
            status = parser_u64(parser, &call_index);
            has_index = status == 0;
        } else if (!strcmp(key, "id")) {
            free(call.id);
            status = parser_nullable_string(parser, &call.id);
        } else if (!strcmp(key, "function")) {
            status = parse_function(parser, &call);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) goto fail;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            break;
        }
        if (parser_expect(parser, ',') != 0) goto fail;
    }
    if (!has_index || call_index > 1024) {
        parser_fail(parser, "invalid tool-call delta index");
        goto fail;
    }
    if (result_add_call_at(result, &call, (size_t)call_index) != 0) {
        parser_fail(parser, "out of memory");
        goto fail;
    }
    return 0;

fail:
    free(call.id);
    free(call.name);
    free(call.arguments_json);
    return -1;
}

static int parse_tool_call_deltas(json_parser *parser,
                                  deepseek_result *result) {
    parser_skip_space(parser);
    if (parser_match(parser, "null")) return 0;
    if (parser_expect(parser, '[') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        if (parse_tool_call_delta(parser, result) != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == ']') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_message(json_parser *parser, deepseek_result *result) {
    if (parser_expect(parser, '{') != 0) return -1;
    result->has_message = 1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status;
        if (!strcmp(key, "content")) {
            free(result->content);
            status = parser_nullable_string(parser, &result->content);
        } else if (!strcmp(key, "reasoning_content")) {
            free(result->reasoning);
            status = parser_nullable_string(parser, &result->reasoning);
        } else if (!strcmp(key, "tool_calls")) {
            status = parse_tool_calls(parser, result);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_delta(json_parser *parser, deepseek_result *result) {
    if (parser_expect(parser, '{') != 0) return -1;
    result->has_delta = 1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status;
        if (!strcmp(key, "content")) {
            free(result->content);
            status = parser_nullable_string(parser, &result->content);
        } else if (!strcmp(key, "reasoning_content")) {
            free(result->reasoning);
            status = parser_nullable_string(parser, &result->reasoning);
        } else if (!strcmp(key, "tool_calls")) {
            status = parse_tool_call_deltas(parser, result);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_choice(json_parser *parser, deepseek_result *result) {
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status;
        if (!strcmp(key, "message")) {
            status = parse_message(parser, result);
        } else if (!strcmp(key, "delta")) {
            status = parse_delta(parser, result);
        } else if (!strcmp(key, "finish_reason")) {
            free(result->finish_reason);
            status = parser_nullable_string(parser, &result->finish_reason);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_choices(json_parser *parser, deepseek_result *result) {
    if (parser_expect(parser, '[') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        return 0;
    }
    size_t index = 0;
    for (;;) {
        int status = index++ == 0 ?
            parse_choice(parser, result) :
            parser_skip_value(parser, 0);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == ']') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_api_error(json_parser *parser, deepseek_result *result) {
    parser_skip_space(parser);
    if (parser->cursor >= parser->end || *parser->cursor != '{')
        return parser_skip_value(parser, 0);
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status;
        if (!strcmp(key, "message")) {
            free(result->error_message);
            status = parser_string(parser, &result->error_message);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_completion_token_details(json_parser *parser,
                                          deepseek_result *result) {
    parser_skip_space(parser);
    if (parser_match(parser, "null")) return 0;
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status = !strcmp(key, "reasoning_tokens") ?
            parser_u64(parser, &result->reasoning_tokens) :
            parser_skip_value(parser, 0);
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_usage(json_parser *parser, deepseek_result *result) {
    parser_skip_space(parser);
    if (parser_match(parser, "null")) return 0;
    if (parser_expect(parser, '{') != 0) return -1;
    result->has_usage = 1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status;
        if (!strcmp(key, "prompt_tokens")) {
            status = parser_u64(parser, &result->prompt_tokens);
        } else if (!strcmp(key, "completion_tokens")) {
            status = parser_u64(parser, &result->completion_tokens);
        } else if (!strcmp(key, "total_tokens")) {
            status = parser_u64(parser, &result->total_tokens);
        } else if (!strcmp(key, "prompt_cache_hit_tokens")) {
            status = parser_u64(parser, &result->prompt_cache_hit_tokens);
        } else if (!strcmp(key, "prompt_cache_miss_tokens")) {
            status = parser_u64(parser, &result->prompt_cache_miss_tokens);
        } else if (!strcmp(key, "completion_tokens_details")) {
            status = parse_completion_token_details(parser, result);
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int parse_response(const char *json, size_t length,
                          deepseek_result *result, char *error,
                          size_t error_length) {
    json_parser parser = {
        .cursor = json,
        .end = json + length
    };
    if (parser_expect(&parser, '{') != 0) goto fail;
    parser_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == '}') {
        parser.cursor++;
    } else {
        for (;;) {
            char *key = NULL;
            if (parser_string(&parser, &key) != 0 ||
                parser_expect(&parser, ':') != 0) {
                free(key);
                goto fail;
            }
            int status;
            if (!strcmp(key, "choices")) {
                status = parse_choices(&parser, result);
            } else if (!strcmp(key, "error")) {
                status = parse_api_error(&parser, result);
            } else if (!strcmp(key, "usage")) {
                status = parse_usage(&parser, result);
            } else if (!strcmp(key, "id")) {
                free(result->request_id);
                status = parser_string(&parser, &result->request_id);
            } else if (!strcmp(key, "model")) {
                free(result->model);
                status = parser_string(&parser, &result->model);
            } else {
                status = parser_skip_value(&parser, 0);
            }
            free(key);
            if (status != 0) goto fail;
            parser_skip_space(&parser);
            if (parser.cursor < parser.end && *parser.cursor == '}') {
                parser.cursor++;
                break;
            }
            if (parser_expect(&parser, ',') != 0) goto fail;
        }
    }
    parser_skip_space(&parser);
    if (parser.cursor != parser.end) {
        parser_fail(&parser, "trailing data after JSON response");
        goto fail;
    }
    if (!result->has_message && !result->has_delta &&
        !result->has_usage && !result->error_message) {
        parser_fail(&parser, "Chat Completions response has no message, delta, or usage");
        goto fail;
    }
    return 0;

fail:
    snprintf(error, error_length, "%s",
             parser.error[0] ? parser.error : "invalid JSON response");
    return -1;
}

static int emit_event(sirio_bridge_event_callback callback,
                      void *private_data,
                      const sirio_bridge_event *event) {
    return callback ? callback(event, private_data) : 0;
}

static int emit_bridge_error(sirio_bridge *bridge,
                             sirio_bridge_event_callback callback,
                             void *private_data) {
    sirio_bridge_event event = {
        .type = SIRIO_BRIDGE_EVENT_ERROR,
        .error = bridge->last_error
    };
    emit_event(callback, private_data, &event);
    return -1;
}

typedef struct {
    sirio_bridge *bridge;
    sirio_bridge_event_callback callback;
    void *private_data;
    bridge_buffer body;
    bridge_buffer pending;
    bridge_buffer event_data;
    deepseek_result calls;
    char *request_id;
    char *model;
    char *finish_reason;
    uint64_t received;
    int content_type_seen;
    int is_sse;
    int failed;
    int callback_rejected;
    int saw_done;
    int saw_postlude;
    int saw_finish;
    int saw_usage;
    int saw_chunk;
    char error[192];
} stream_response;

static int stream_fail(stream_response *response, const char *format, ...) {
    if (!response->error[0]) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(response->error, sizeof(response->error), format, arguments);
        va_end(arguments);
    }
    response->failed = 1;
    return -1;
}

static bool bridge_utf8_valid(const char *text) {
    if (!text) return true;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p <= 0x7f) {
            p++;
            continue;
        }
        if (*p >= 0xc2 && *p <= 0xdf) {
            if (!p[1]) return false;
            if ((p[1] & 0xc0) != 0x80) return false;
            p += 2;
            continue;
        }
        if (*p == 0xe0) {
            if (!p[1] || !p[2]) return false;
            if (p[1] < 0xa0 || p[1] > 0xbf ||
                (p[2] & 0xc0) != 0x80) return false;
            p += 3;
            continue;
        }
        if ((*p >= 0xe1 && *p <= 0xec) ||
            (*p >= 0xee && *p <= 0xef)) {
            if (!p[1] || !p[2]) return false;
            if ((p[1] & 0xc0) != 0x80 ||
                (p[2] & 0xc0) != 0x80) return false;
            p += 3;
            continue;
        }
        if (*p == 0xed) {
            if (!p[1] || !p[2]) return false;
            if (p[1] < 0x80 || p[1] > 0x9f ||
                (p[2] & 0xc0) != 0x80) return false;
            p += 3;
            continue;
        }
        if (*p == 0xf0) {
            if (!p[1] || !p[2] || !p[3]) return false;
            if (p[1] < 0x90 || p[1] > 0xbf ||
                (p[2] & 0xc0) != 0x80 ||
                (p[3] & 0xc0) != 0x80) return false;
            p += 4;
            continue;
        }
        if (*p >= 0xf1 && *p <= 0xf3) {
            if (!p[1] || !p[2] || !p[3]) return false;
            if ((p[1] & 0xc0) != 0x80 ||
                (p[2] & 0xc0) != 0x80 ||
                (p[3] & 0xc0) != 0x80) return false;
            p += 4;
            continue;
        }
        if (*p == 0xf4) {
            if (!p[1] || !p[2] || !p[3]) return false;
            if (p[1] < 0x80 || p[1] > 0x8f ||
                (p[2] & 0xc0) != 0x80 ||
                (p[3] & 0xc0) != 0x80) return false;
            p += 4;
            continue;
        }
        return false;
    }
    return true;
}

void sirio_tool_arguments_free(sirio_tool_argument *arguments,
                               size_t argument_count) {
    for (size_t i = 0; i < argument_count; i++) {
        free(arguments[i].name);
        free(arguments[i].value);
    }
    free(arguments);
}

static void tool_arguments_copy_error(char *error, size_t error_len,
                                      const char *message) {
    if (!error || !error_len) return;
    snprintf(error, error_len, "%s", message && message[0] ? message :
             "invalid tool arguments");
}

int sirio_tool_arguments_parse(const char *json,
                               sirio_tool_argument **arguments_out,
                               size_t *argument_count_out,
                               char *error, size_t error_len) {
    if (arguments_out) *arguments_out = NULL;
    if (argument_count_out) *argument_count_out = 0;
    if (error && error_len) error[0] = '\0';
    if (!json || !arguments_out || !argument_count_out) {
        tool_arguments_copy_error(error, error_len,
                                  "missing tool arguments input");
        return -1;
    }

    json_parser parser = {
        .cursor = json,
        .end = json + strlen(json),
    };
    sirio_tool_argument *arguments = NULL;
    size_t count = 0, capacity = 0;
    if (parser_expect(&parser, '{') != 0) goto fail;
    parser_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == '}') {
        parser.cursor++;
        goto complete;
    }

    for (;;) {
        char *name = NULL;
        char *value = NULL;
        sirio_tool_argument_type type = SIRIO_TOOL_ARGUMENT_NUMBER;
        if (parser_string(&parser, &name) != 0 ||
            parser_expect(&parser, ':') != 0)
            goto item_fail;
        if (!name[0] || !bridge_utf8_valid(name)) {
            parser_fail(&parser, "invalid tool argument name");
            goto item_fail;
        }
        for (size_t i = 0; i < count; i++) {
            if (!strcmp(arguments[i].name, name)) {
                parser_fail(&parser, "duplicate tool argument name");
                goto item_fail;
            }
        }

        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == '"') {
            if (parser_string(&parser, &value) != 0) goto item_fail;
            type = SIRIO_TOOL_ARGUMENT_STRING;
        } else if (parser_match(&parser, "true")) {
            value = bridge_strdup("true");
            type = SIRIO_TOOL_ARGUMENT_BOOLEAN;
        } else if (parser_match(&parser, "false")) {
            value = bridge_strdup("false");
            type = SIRIO_TOOL_ARGUMENT_BOOLEAN;
        } else if (parser.cursor < parser.end &&
                   (*parser.cursor == '-' ||
                    isdigit((unsigned char)*parser.cursor))) {
            const char *start = parser.cursor;
            if (parser_skip_number(&parser) != 0) goto item_fail;
            size_t length = (size_t)(parser.cursor - start);
            value = malloc(length + 1);
            if (value) {
                memcpy(value, start, length);
                value[length] = '\0';
            }
        } else {
            parser_fail(&parser,
                        "tool argument values must be strings, numbers, or booleans");
            goto item_fail;
        }
        if (!value) {
            parser_fail(&parser, "out of memory");
            goto item_fail;
        }
        if (!bridge_utf8_valid(value)) {
            parser_fail(&parser, "invalid UTF-8 in tool argument value");
            goto item_fail;
        }
        if (count == 128) {
            parser_fail(&parser, "too many tool arguments");
            goto item_fail;
        }
        if (count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 8;
            sirio_tool_argument *next = realloc(
                arguments, next_capacity * sizeof(*next));
            if (!next) {
                parser_fail(&parser, "out of memory");
                goto item_fail;
            }
            arguments = next;
            capacity = next_capacity;
        }
        arguments[count++] = (sirio_tool_argument) {
            .name = name,
            .value = value,
            .type = type,
        };
        name = NULL;
        value = NULL;

        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == '}') {
            parser.cursor++;
            break;
        }
        if (parser_expect(&parser, ',') != 0) goto fail;
        continue;

item_fail:
        free(name);
        free(value);
        goto fail;
    }

complete:
    parser_skip_space(&parser);
    if (parser.cursor != parser.end) {
        parser_fail(&parser, "trailing data after tool arguments object");
        goto fail;
    }
    *arguments_out = arguments;
    *argument_count_out = count;
    return 0;

fail:
    tool_arguments_copy_error(error, error_len, parser.error);
    sirio_tool_arguments_free(arguments, count);
    return -1;
}

typedef enum {
    TOOL_SCHEMA_STRING,
    TOOL_SCHEMA_NUMBER,
    TOOL_SCHEMA_INTEGER,
    TOOL_SCHEMA_BOOLEAN,
} tool_schema_type;

typedef struct {
    char *name;
    tool_schema_type type;
    bool required;
} tool_schema_property;

typedef struct {
    tool_schema_property *properties;
    size_t property_count;
    size_t property_capacity;
    bool additional_properties;
} tool_schema;

static void tool_schema_free(tool_schema *schema) {
    for (size_t i = 0; i < schema->property_count; i++)
        free(schema->properties[i].name);
    free(schema->properties);
    memset(schema, 0, sizeof(*schema));
}

static int tool_schema_type_parse(json_parser *parser,
                                  tool_schema_type *type) {
    char *name = NULL;
    if (parser_string(parser, &name) != 0) return -1;
    int result = 0;
    if (!strcmp(name, "string")) *type = TOOL_SCHEMA_STRING;
    else if (!strcmp(name, "number")) *type = TOOL_SCHEMA_NUMBER;
    else if (!strcmp(name, "integer")) *type = TOOL_SCHEMA_INTEGER;
    else if (!strcmp(name, "boolean")) *type = TOOL_SCHEMA_BOOLEAN;
    else result = parser_fail(parser, "unsupported tool schema property type");
    free(name);
    return result;
}

static int tool_schema_property_parse(json_parser *parser,
                                      tool_schema_type *type) {
    if (parser_expect(parser, '{') != 0) return -1;
    bool saw_type = false;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return parser_fail(parser, "tool schema property has no type");
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(parser, &key) != 0 ||
            parser_expect(parser, ':') != 0) {
            free(key);
            return -1;
        }
        int status;
        if (!strcmp(key, "type")) {
            if (saw_type) {
                free(key);
                return parser_fail(parser,
                                   "duplicate type in tool schema property");
            }
            status = tool_schema_type_parse(parser, type);
            saw_type = status == 0;
        } else {
            status = parser_skip_value(parser, 0);
        }
        free(key);
        if (status != 0) return -1;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            break;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
    return saw_type ? 0 : parser_fail(parser,
                                      "tool schema property has no type");
}

static int tool_schema_add_property(json_parser *parser,
                                    tool_schema *schema, char *name,
                                    tool_schema_type type) {
    if (!name[0] || !bridge_utf8_valid(name))
        return parser_fail(parser, "invalid tool schema property name");
    for (size_t i = 0; i < schema->property_count; i++)
        if (!strcmp(schema->properties[i].name, name))
            return parser_fail(parser, "duplicate tool schema property");
    if (schema->property_count == 128)
        return parser_fail(parser, "too many tool schema properties");
    if (schema->property_count == schema->property_capacity) {
        size_t capacity = schema->property_capacity ?
                          schema->property_capacity * 2 : 8;
        tool_schema_property *replacement = realloc(
            schema->properties, capacity * sizeof(*replacement));
        if (!replacement) return parser_fail(parser, "out of memory");
        schema->properties = replacement;
        schema->property_capacity = capacity;
    }
    schema->properties[schema->property_count++] =
        (tool_schema_property){.name = name, .type = type};
    return 0;
}

static int tool_schema_properties_parse(json_parser *parser,
                                        tool_schema *schema) {
    if (parser_expect(parser, '{') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return 0;
    }
    for (;;) {
        char *name = NULL;
        tool_schema_type type = TOOL_SCHEMA_STRING;
        if (parser_string(parser, &name) != 0 ||
            parser_expect(parser, ':') != 0 ||
            tool_schema_property_parse(parser, &type) != 0 ||
            tool_schema_add_property(parser, schema, name, type) != 0) {
            free(name);
            return -1;
        }
        name = NULL;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == '}') {
            parser->cursor++;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) return -1;
    }
}

static int tool_schema_required_parse(json_parser *parser,
                                      char ***names_out, size_t *count_out) {
    char **names = NULL;
    size_t count = 0, capacity = 0;
    if (parser_expect(parser, '[') != 0) return -1;
    parser_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        *names_out = NULL;
        *count_out = 0;
        return 0;
    }
    for (;;) {
        char *name = NULL;
        if (parser_string(parser, &name) != 0) goto fail;
        if (!name[0] || !bridge_utf8_valid(name)) {
            parser_fail(parser, "invalid required tool property name");
            free(name);
            goto fail;
        }
        for (size_t i = 0; i < count; i++) {
            if (!strcmp(names[i], name)) {
                parser_fail(parser, "duplicate required tool property");
                free(name);
                goto fail;
            }
        }
        if (count == 128) {
            parser_fail(parser, "too many required tool properties");
            free(name);
            goto fail;
        }
        if (count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 8;
            char **replacement = realloc(
                names, next_capacity * sizeof(*replacement));
            if (!replacement) {
                parser_fail(parser, "out of memory");
                free(name);
                goto fail;
            }
            names = replacement;
            capacity = next_capacity;
        }
        names[count++] = name;
        parser_skip_space(parser);
        if (parser->cursor < parser->end && *parser->cursor == ']') {
            parser->cursor++;
            *names_out = names;
            *count_out = count;
            return 0;
        }
        if (parser_expect(parser, ',') != 0) goto fail;
    }

fail:
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return -1;
}

static int tool_schema_boolean_parse(json_parser *parser, bool *value) {
    if (parser_match(parser, "true")) {
        *value = true;
        return 0;
    }
    if (parser_match(parser, "false")) {
        *value = false;
        return 0;
    }
    return parser_fail(parser, "expected tool schema boolean");
}

static int tool_schema_parse(const char *json, tool_schema *schema,
                             char *error, size_t error_len) {
    json_parser parser = {
        .cursor = json,
        .end = json ? json + strlen(json) : NULL,
    };
    char **required = NULL;
    size_t required_count = 0;
    bool saw_root_type = false, root_is_object = false;
    bool saw_properties = false, saw_required = false;
    bool saw_additional = false;
    schema->additional_properties = true;
    if (!json || parser_expect(&parser, '{') != 0) goto fail;
    parser_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == '}') {
        parser.cursor++;
        parser_fail(&parser, "tool schema has no object type");
        goto fail;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(&parser, &key) != 0 ||
            parser_expect(&parser, ':') != 0) {
            free(key);
            goto fail;
        }
        int status = 0;
        if (!strcmp(key, "type")) {
            char *type = NULL;
            if (saw_root_type) status = parser_fail(
                &parser, "duplicate root type in tool schema");
            else if (parser_string(&parser, &type) != 0) status = -1;
            else {
                saw_root_type = true;
                root_is_object = !strcmp(type, "object");
                if (!root_is_object)
                    status = parser_fail(&parser,
                                         "tool schema root must be object");
            }
            free(type);
        } else if (!strcmp(key, "properties")) {
            if (saw_properties)
                status = parser_fail(&parser,
                                     "duplicate properties in tool schema");
            else {
                saw_properties = true;
                status = tool_schema_properties_parse(&parser, schema);
            }
        } else if (!strcmp(key, "required")) {
            if (saw_required)
                status = parser_fail(&parser,
                                     "duplicate required in tool schema");
            else {
                saw_required = true;
                status = tool_schema_required_parse(
                    &parser, &required, &required_count);
            }
        } else if (!strcmp(key, "additionalProperties")) {
            if (saw_additional)
                status = parser_fail(
                    &parser, "duplicate additionalProperties in tool schema");
            else {
                saw_additional = true;
                status = tool_schema_boolean_parse(
                    &parser, &schema->additional_properties);
            }
        } else {
            status = parser_skip_value(&parser, 0);
        }
        free(key);
        if (status != 0) goto fail;
        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == '}') {
            parser.cursor++;
            break;
        }
        if (parser_expect(&parser, ',') != 0) goto fail;
    }
    parser_skip_space(&parser);
    if (parser.cursor != parser.end) {
        parser_fail(&parser, "trailing data after tool schema");
        goto fail;
    }
    if (!saw_root_type || !root_is_object || !saw_properties) {
        parser_fail(&parser,
                    "tool schema requires object type and properties");
        goto fail;
    }
    for (size_t i = 0; i < required_count; i++) {
        tool_schema_property *match = NULL;
        for (size_t j = 0; j < schema->property_count; j++)
            if (!strcmp(required[i], schema->properties[j].name))
                match = &schema->properties[j];
        if (!match) {
            parser_fail(&parser,
                        "required name is absent from tool schema properties");
            goto fail;
        }
        match->required = true;
    }
    for (size_t i = 0; i < required_count; i++) free(required[i]);
    free(required);
    return 0;

fail:
    for (size_t i = 0; i < required_count; i++) free(required[i]);
    free(required);
    tool_arguments_copy_error(error, error_len,
                              parser.error[0] ? parser.error :
                              "invalid tool input schema");
    tool_schema_free(schema);
    return -1;
}

static const char *tool_argument_type_name(sirio_tool_argument_type type) {
    switch (type) {
    case SIRIO_TOOL_ARGUMENT_STRING: return "string";
    case SIRIO_TOOL_ARGUMENT_NUMBER: return "number";
    case SIRIO_TOOL_ARGUMENT_BOOLEAN: return "boolean";
    }
    return "unknown";
}

static int tool_argument_normalize_integer(sirio_tool_argument *argument) {
    const char *text = argument && argument->value ? argument->value : "";
    bool negative = *text == '-';
    if (negative) text++;
    bridge_buffer digits = {0};
    size_t fractional = 0;
    while (isdigit((unsigned char)*text)) {
        if (buffer_append(&digits, text++, 1) != 0) goto fail;
    }
    if (*text == '.') {
        text++;
        const char *fraction_start = text;
        while (isdigit((unsigned char)*text)) {
            if (buffer_append(&digits, text++, 1) != 0) goto fail;
        }
        fractional = (size_t)(text - fraction_start);
    }
    long exponent = 0;
    if (*text == 'e' || *text == 'E') {
        text++;
        bool exponent_negative = *text == '-';
        if (*text == '+' || *text == '-') text++;
        while (isdigit((unsigned char)*text)) {
            int digit = *text++ - '0';
            if (exponent < 1000000)
                exponent = exponent * 10 + digit;
        }
        if (exponent_negative) exponent = -exponent;
    }
    if (*text || !digits.length) goto fail;

    size_t first = 0;
    while (first < digits.length && digits.data[first] == '0') first++;
    if (first == digits.length) {
        buffer_free(&digits);
        char *replacement = bridge_strdup("0");
        if (!replacement) return -1;
        free(argument->value);
        argument->value = replacement;
        return 0;
    }
    if (first) {
        memmove(digits.data, digits.data + first, digits.length - first);
        digits.length -= first;
        digits.data[digits.length] = '\0';
    }

    long scale;
    if (fractional > (size_t)LONG_MAX ||
        exponent < LONG_MIN + (long)fractional)
        goto fail;
    scale = exponent - (long)fractional;
    if (scale < 0) {
        unsigned long remove = (unsigned long)(-(scale + 1)) + 1;
        if (remove > digits.length) goto fail;
        for (size_t i = digits.length - (size_t)remove;
             i < digits.length; i++)
            if (digits.data[i] != '0') goto fail;
        digits.length -= (size_t)remove;
        digits.data[digits.length] = '\0';
        if (!digits.length) {
            buffer_free(&digits);
            char *replacement = bridge_strdup("0");
            if (!replacement) return -1;
            free(argument->value);
            argument->value = replacement;
            return 0;
        }
    } else if (scale > 0) {
        if (scale > 19 || buffer_reserve(&digits, (size_t)scale) != 0)
            goto fail;
        memset(digits.data + digits.length, '0', (size_t)scale);
        digits.length += (size_t)scale;
        digits.data[digits.length] = '\0';
    }

    const char *limit = negative ? "9223372036854775808" :
                                   "9223372036854775807";
    if (digits.length > 19 ||
        (digits.length == 19 && strcmp(digits.data, limit) > 0))
        goto fail;
    bridge_buffer normalized = {0};
    if (negative && buffer_putc(&normalized, '-') != 0) goto fail_normalized;
    if (buffer_append(&normalized, digits.data, digits.length) != 0)
        goto fail_normalized;
    char *replacement = buffer_release(&normalized);
    if (!replacement) return -1;
    buffer_free(&digits);
    free(argument->value);
    argument->value = replacement;
    return 0;

fail_normalized:
    buffer_free(&normalized);
fail:
    buffer_free(&digits);
    return -1;
}

int sirio_tool_arguments_parse_validated(
        const char *arguments_json, const char *input_schema_json,
        sirio_tool_argument **arguments_out, size_t *argument_count_out,
        char *error, size_t error_len) {
    if (arguments_out) *arguments_out = NULL;
    if (argument_count_out) *argument_count_out = 0;
    sirio_tool_argument *arguments = NULL;
    size_t argument_count = 0;
    if (sirio_tool_arguments_parse(arguments_json, &arguments,
                                   &argument_count, error, error_len) != 0)
        return -1;
    tool_schema schema = {0};
    if (tool_schema_parse(input_schema_json, &schema,
                          error, error_len) != 0) {
        sirio_tool_arguments_free(arguments, argument_count);
        return -1;
    }

    for (size_t i = 0; i < argument_count; i++) {
        const tool_schema_property *property = NULL;
        for (size_t j = 0; j < schema.property_count; j++)
            if (!strcmp(arguments[i].name, schema.properties[j].name))
                property = &schema.properties[j];
        if (!property) {
            if (schema.additional_properties) continue;
            if (error && error_len)
                snprintf(error, error_len, "unknown tool argument: %.96s",
                         arguments[i].name);
            goto fail;
        }
        bool matches =
            (property->type == TOOL_SCHEMA_STRING &&
             arguments[i].type == SIRIO_TOOL_ARGUMENT_STRING) ||
            ((property->type == TOOL_SCHEMA_NUMBER ||
              property->type == TOOL_SCHEMA_INTEGER) &&
             arguments[i].type == SIRIO_TOOL_ARGUMENT_NUMBER) ||
            (property->type == TOOL_SCHEMA_BOOLEAN &&
             arguments[i].type == SIRIO_TOOL_ARGUMENT_BOOLEAN);
        if (!matches) {
            const char *expected = property->type == TOOL_SCHEMA_STRING ?
                "string" : property->type == TOOL_SCHEMA_BOOLEAN ?
                "boolean" : property->type == TOOL_SCHEMA_INTEGER ?
                "integer" : "number";
            if (error && error_len)
                snprintf(error, error_len,
                         "tool argument %.80s must be %s, not %s",
                         arguments[i].name, expected,
                         tool_argument_type_name(arguments[i].type));
            goto fail;
        }
        if (property->type == TOOL_SCHEMA_INTEGER &&
            tool_argument_normalize_integer(&arguments[i]) != 0) {
            if (error && error_len)
                snprintf(error, error_len,
                         "tool argument %.80s must be a portable integer",
                         arguments[i].name);
            goto fail;
        }
    }
    for (size_t i = 0; i < schema.property_count; i++) {
        if (!schema.properties[i].required) continue;
        bool found = false;
        for (size_t j = 0; j < argument_count; j++)
            if (!strcmp(schema.properties[i].name, arguments[j].name))
                found = true;
        if (!found) {
            if (error && error_len)
                snprintf(error, error_len,
                         "missing required tool argument: %.96s",
                         schema.properties[i].name);
            goto fail;
        }
    }

    tool_schema_free(&schema);
    *arguments_out = arguments;
    *argument_count_out = argument_count;
    return 0;

fail:
    tool_schema_free(&schema);
    sirio_tool_arguments_free(arguments, argument_count);
    return -1;
}

static int stream_emit(stream_response *response,
                       const sirio_bridge_event *event) {
    if (!response->callback) return 0;
    if (response->callback(event, response->private_data) == 0) return 0;
    response->callback_rejected = 1;
    return stream_fail(response, "agent rejected a Chat Completions event");
}

static int stream_set_identity(stream_response *response, char **stored,
                               const char *value, const char *label) {
    if (!value) return 0;
    if (!bridge_utf8_valid(value))
        return stream_fail(response, "invalid UTF-8 in Chat Completions %s", label);
    if (*stored) {
        if (strcmp(*stored, value))
            return stream_fail(response, "Chat Completions %s changed during stream",
                               label);
        return 0;
    }
    *stored = bridge_strdup(value);
    if (!*stored) return stream_fail(response, "out of memory");
    return 0;
}

static int stream_append_fragment(stream_response *response,
                                  char **stored, const char *fragment,
                                  const char *label) {
    if (!fragment) return 0;
    if (!bridge_utf8_valid(fragment))
        return stream_fail(response, "invalid UTF-8 in Chat Completions %s", label);
    size_t old_length = *stored ? strlen(*stored) : 0;
    size_t fragment_length = strlen(fragment);
    if (fragment_length > DEEPSEEK_RESPONSE_LIMIT - old_length)
        return stream_fail(response, "Chat Completions %s exceeded limits", label);
    char *replacement = realloc(*stored, old_length + fragment_length + 1);
    if (!replacement) return stream_fail(response, "out of memory");
    memcpy(replacement + old_length, fragment, fragment_length + 1);
    *stored = replacement;
    return 0;
}

static sirio_tool_call *stream_call_for_index(stream_response *response,
                                               size_t call_index) {
    for (size_t i = 0; i < response->calls.call_count; i++) {
        if (response->calls.call_indices[i] == call_index)
            return &response->calls.calls[i];
    }
    sirio_tool_call call = {0};
    if (result_add_call_at(&response->calls, &call, call_index) != 0) {
        stream_fail(response, "out of memory");
        return NULL;
    }
    return &response->calls.calls[response->calls.call_count - 1];
}

static int stream_accumulate_calls(stream_response *response,
                                   const deepseek_result *chunk) {
    for (size_t i = 0; i < chunk->call_count; i++) {
        size_t call_index = chunk->call_indices[i];
        sirio_tool_call *call = stream_call_for_index(response, call_index);
        if (!call) return -1;
        if (stream_append_fragment(response, &call->id,
                                   chunk->calls[i].id, "tool-call id") != 0 ||
            stream_append_fragment(response, &call->name,
                                   chunk->calls[i].name,
                                   "tool-call name") != 0 ||
            stream_append_fragment(response, &call->arguments_json,
                                   chunk->calls[i].arguments_json,
                                   "tool-call arguments") != 0)
            return -1;
    }
    return 0;
}

static int stream_dispatch_json(stream_response *response,
                                const char *json, size_t json_length) {
    deepseek_result chunk = {0};
    char parse_error[160] = {0};
    if (parse_response(json, json_length, &chunk,
                       parse_error, sizeof(parse_error)) != 0) {
        result_free(&chunk);
        return stream_fail(response, "malformed Chat Completions stream chunk: %s",
                           parse_error);
    }
    response->saw_chunk = 1;
    if (chunk.error_message) {
        stream_fail(response, "Chat Completions stream error: %s",
                    chunk.error_message);
        result_free(&chunk);
        return -1;
    }
    if (stream_set_identity(response, &response->request_id,
                            chunk.request_id, "request id") != 0 ||
        stream_set_identity(response, &response->model,
                            chunk.model, "model") != 0) {
        result_free(&chunk);
        return -1;
    }
    if (chunk.reasoning && chunk.reasoning[0]) {
        if (!bridge_utf8_valid(chunk.reasoning)) {
            result_free(&chunk);
            return stream_fail(response,
                               "invalid UTF-8 in Chat Completions reasoning delta");
        }
        sirio_bridge_event event = {
            .type = SIRIO_BRIDGE_EVENT_REASONING,
            .text = chunk.reasoning,
            .request_id = response->request_id,
            .model = response->model,
        };
        if (stream_emit(response, &event) != 0) {
            result_free(&chunk);
            return -1;
        }
    }
    if (chunk.content && chunk.content[0]) {
        if (!bridge_utf8_valid(chunk.content)) {
            result_free(&chunk);
            return stream_fail(response,
                               "invalid UTF-8 in Chat Completions content delta");
        }
        sirio_bridge_event event = {
            .type = SIRIO_BRIDGE_EVENT_TEXT,
            .text = chunk.content,
            .request_id = response->request_id,
            .model = response->model,
        };
        if (stream_emit(response, &event) != 0) {
            result_free(&chunk);
            return -1;
        }
    }
    if (stream_accumulate_calls(response, &chunk) != 0) {
        result_free(&chunk);
        return -1;
    }
    if (chunk.finish_reason) {
        if (!bridge_utf8_valid(chunk.finish_reason)) {
            result_free(&chunk);
            return stream_fail(response,
                               "invalid UTF-8 in Chat Completions finish reason");
        }
        if (response->saw_finish &&
            strcmp(response->finish_reason, chunk.finish_reason)) {
            result_free(&chunk);
            return stream_fail(response,
                               "Chat Completions finish reason changed during stream");
        }
        if (!response->saw_finish) {
            response->finish_reason = bridge_strdup(chunk.finish_reason);
            if (!response->finish_reason) {
                result_free(&chunk);
                return stream_fail(response, "out of memory");
            }
            response->saw_finish = 1;
        }
    }
    if (chunk.has_usage) {
        if (response->saw_usage) {
            result_free(&chunk);
            return stream_fail(response,
                               "duplicate Chat Completions stream usage chunk");
        }
        response->saw_usage = 1;
        sirio_bridge_event event = {
            .type = SIRIO_BRIDGE_EVENT_USAGE,
            .request_id = response->request_id,
            .model = response->model,
            .prompt_tokens = chunk.prompt_tokens,
            .completion_tokens = chunk.completion_tokens,
            .total_tokens = chunk.total_tokens,
            .reasoning_tokens = chunk.reasoning_tokens,
            .prompt_cache_hit_tokens = chunk.prompt_cache_hit_tokens,
            .prompt_cache_miss_tokens = chunk.prompt_cache_miss_tokens,
        };
        if (stream_emit(response, &event) != 0) {
            result_free(&chunk);
            return -1;
        }
    }
    result_free(&chunk);
    return 0;
}

static int opencode_postlude_is_valid(const char *json, size_t length) {
    json_parser parser = {.cursor = json, .end = json + length};
    bool saw_choices = false;
    bool saw_cost = false;

    if (parser_expect(&parser, '{') != 0) return 0;
    parser_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == '}') return 0;
    for (;;) {
        char *key = NULL;
        if (parser_string(&parser, &key) != 0 ||
            parser_expect(&parser, ':') != 0) {
            free(key);
            return 0;
        }
        if (!strcmp(key, "choices") && !saw_choices) {
            saw_choices = true;
            free(key);
            if (parser_expect(&parser, '[') != 0 ||
                parser_expect(&parser, ']') != 0)
                return 0;
        } else if (!strcmp(key, "cost") && !saw_cost) {
            saw_cost = true;
            free(key);
            parser_skip_space(&parser);
            if (parser.cursor < parser.end && *parser.cursor == '"') {
                char *cost = NULL;
                if (parser_string(&parser, &cost) != 0) {
                    free(cost);
                    return 0;
                }
                free(cost);
            } else if (parser_skip_number(&parser) != 0) {
                return 0;
            }
        } else {
            free(key);
            return 0;
        }
        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == '}') {
            parser.cursor++;
            break;
        }
        if (parser_expect(&parser, ',') != 0) return 0;
    }
    parser_skip_space(&parser);
    return saw_choices && saw_cost && parser.cursor == parser.end;
}

static int stream_dispatch_event(stream_response *response) {
    const char *data = response->event_data.data ?
        response->event_data.data : "";
    size_t length = response->event_data.length;
    while (length && isspace((unsigned char)*data)) {
        data++;
        length--;
    }
    while (length && isspace((unsigned char)data[length - 1])) length--;
    if (!length) {
        response->event_data.length = 0;
        if (response->event_data.data) response->event_data.data[0] = '\0';
        return 0;
    }
    int status;
    if (response->saw_done) {
        if (response->bridge &&
            response->bridge->provider_id == SIRIO_PROVIDER_OPENCODE_GO &&
            !response->saw_postlude &&
            opencode_postlude_is_valid(data, length)) {
            response->saw_postlude = 1;
            status = 0;
        } else {
            return stream_fail(response,
                               "data after Chat Completions [DONE]");
        }
    } else if (length == 6 && !memcmp(data, "[DONE]", 6)) {
        response->saw_done = 1;
        status = 0;
    } else {
        status = stream_dispatch_json(response, data, length);
    }
    response->event_data.length = 0;
    if (response->event_data.data) response->event_data.data[0] = '\0';
    return status;
}

static int stream_process_line(stream_response *response,
                               const char *line, size_t length) {
    if (!length) return stream_dispatch_event(response);
    if (line[0] == ':') return 0;
    const char *colon = memchr(line, ':', length);
    size_t field_length = colon ? (size_t)(colon - line) : length;
    if (field_length != 4 || memcmp(line, "data", 4)) return 0;
    const char *value = colon ? colon + 1 : line + length;
    size_t value_length = colon ?
        length - (size_t)(value - line) : 0;
    if (value_length && *value == ' ') {
        value++;
        value_length--;
    }
    if (response->event_data.length) {
        if (response->event_data.length >= DEEPSEEK_RESPONSE_LIMIT)
            return stream_fail(response,
                               "Chat Completions SSE event exceeded limits");
        if (buffer_putc(&response->event_data, '\n') != 0)
            return stream_fail(response, "out of memory");
    }
    if (value_length > DEEPSEEK_RESPONSE_LIMIT -
                       response->event_data.length ||
        buffer_append(&response->event_data, value, value_length) != 0)
        return stream_fail(response, "Chat Completions SSE event exceeded limits");
    return 0;
}

static int stream_feed_sse(stream_response *response,
                            const char *data, size_t length) {
    if (length > DEEPSEEK_RESPONSE_LIMIT - response->pending.length ||
        buffer_append(&response->pending, data, length) != 0)
        return stream_fail(response, "Chat Completions SSE buffer exceeded limits");

    size_t consumed = 0;
    while (consumed < response->pending.length) {
        size_t end = consumed;
        while (end < response->pending.length &&
               response->pending.data[end] != '\n' &&
               response->pending.data[end] != '\r')
            end++;
        if (end == response->pending.length) break;
        if (response->pending.data[end] == '\r' &&
            end + 1 == response->pending.length)
            break;
        size_t terminator = 1;
        if (response->pending.data[end] == '\r' &&
            end + 1 < response->pending.length &&
            response->pending.data[end + 1] == '\n')
            terminator = 2;
        if (stream_process_line(response,
                                response->pending.data + consumed,
                                end - consumed) != 0)
            return -1;
        consumed = end + terminator;
    }
    if (consumed) {
        size_t remaining = response->pending.length - consumed;
        memmove(response->pending.data,
                response->pending.data + consumed, remaining);
        response->pending.length = remaining;
        response->pending.data[remaining] = '\0';
    }
    return 0;
}

static size_t stream_http_header(char *data, size_t size, size_t count,
                                 void *private_data) {
    stream_response *response = private_data;
    if (size && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    static const char prefix[] = "Content-Type:";
    if (length >= sizeof(prefix) - 1 &&
        !strncasecmp(data, prefix, sizeof(prefix) - 1)) {
        response->content_type_seen = 1;
        response->is_sse = false;
        const char *value = data + sizeof(prefix) - 1;
        size_t value_length = length - (sizeof(prefix) - 1);
        while (value_length && isspace((unsigned char)*value)) {
            value++;
            value_length--;
        }
        static const char sse_type[] = "text/event-stream";
        if (value_length >= sizeof(sse_type) - 1 &&
            !strncasecmp(value, sse_type, sizeof(sse_type) - 1))
            response->is_sse = true;
    }
    return length;
}

static size_t stream_http_write(char *data, size_t size, size_t count,
                                void *private_data) {
    stream_response *response = private_data;
    if (size && count > SIZE_MAX / size) {
        stream_fail(response, "Chat Completions response size overflow");
        return 0;
    }
    size_t length = size * count;
    if (length > DEEPSEEK_RESPONSE_LIMIT - response->received) {
        stream_fail(response, "Chat Completions response exceeded limits");
        return 0;
    }
    response->received += length;
    if (response->is_sse) {
        if (stream_feed_sse(response, data, length) != 0) return 0;
    } else if (buffer_append(&response->body, data, length) != 0) {
        stream_fail(response, "out of memory buffering Chat Completions response");
        return 0;
    }
    return length;
}

static int stream_finish(stream_response *response) {
    if (response->pending.length || response->event_data.length)
        return stream_fail(response, "truncated Chat Completions SSE event");
    if (!response->saw_chunk)
        return stream_fail(response, "Chat Completions stream contained no chunks");
    if (!response->saw_finish)
        return stream_fail(response, "Chat Completions stream has no finish reason");
    if (!response->finish_reason || !response->finish_reason[0])
        return stream_fail(response, "Chat Completions stream has an empty finish reason");
    if (!response->saw_usage)
        return stream_fail(response, "Chat Completions stream has no usage chunk");
    if (!response->saw_done)
        return stream_fail(response, "Chat Completions stream ended without [DONE]");

    bool tool_finish = !strcmp(response->finish_reason, "tool_calls");
    if (tool_finish && response->calls.call_count == 0)
        return stream_fail(response,
                           "Chat Completions finished with tool_calls but delivered no calls");
    if (!tool_finish && response->calls.call_count != 0)
        return stream_fail(response,
                           "Chat Completions delivered tool calls with a non-tool finish reason");

    for (size_t call_index = 0;
         call_index < response->calls.call_count; call_index++) {
        sirio_tool_call *call = NULL;
        for (size_t i = 0; i < response->calls.call_count; i++) {
            if (response->calls.call_indices[i] == call_index) {
                call = &response->calls.calls[i];
                break;
            }
        }
        if (!call || !call->id || !call->id[0] ||
            !call->name || !call->name[0] || !call->arguments_json)
            return stream_fail(response,
                               "incomplete Chat Completions tool-call stream");
        for (size_t i = 0; i < call_index; i++) {
            sirio_tool_call *prior = NULL;
            for (size_t j = 0; j < response->calls.call_count; j++) {
                if (response->calls.call_indices[j] == i) {
                    prior = &response->calls.calls[j];
                    break;
                }
            }
            if (prior && prior->id && !strcmp(prior->id, call->id))
                return stream_fail(response,
                                   "duplicate Chat Completions tool-call id");
        }
        sirio_bridge_event event = {
            .type = SIRIO_BRIDGE_EVENT_TOOL_CALL,
            .tool_call_id = call->id,
            .tool_name = call->name,
            .tool_arguments_json = call->arguments_json,
            .request_id = response->request_id,
            .model = response->model,
        };
        if (stream_emit(response, &event) != 0) return -1;
    }
    sirio_bridge_event event = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .finish_reason = response->finish_reason,
        .request_id = response->request_id,
        .model = response->model,
    };
    return stream_emit(response, &event);
}

static void stream_response_free(stream_response *response) {
    buffer_free(&response->body);
    buffer_free(&response->pending);
    buffer_free(&response->event_data);
    result_free(&response->calls);
    free(response->request_id);
    free(response->model);
    free(response->finish_reason);
    memset(response, 0, sizeof(*response));
}

/* ------------------------------------------------------------------------- */
/* OpenAI OAuth and Responses adapter.                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *start;
    const char *end;
} json_span;

static int json_object_member(const char *json, size_t length,
                              const char *wanted, json_span *span) {
    json_parser parser = {.cursor = json, .end = json + length};
    if (parser_expect(&parser, '{') != 0) return -1;
    parser_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == '}') return 0;
    for (;;) {
        char *key = NULL;
        if (parser_string(&parser, &key) != 0 ||
            parser_expect(&parser, ':') != 0) {
            free(key);
            return -1;
        }
        parser_skip_space(&parser);
        const char *start = parser.cursor;
        if (parser_skip_value(&parser, 0) != 0) {
            free(key);
            return -1;
        }
        if (!strcmp(key, wanted)) {
            span->start = start;
            span->end = parser.cursor;
            free(key);
            return 1;
        }
        free(key);
        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == '}') return 0;
        if (parser_expect(&parser, ',') != 0) return -1;
    }
}

static int json_object_shape(const char *json, size_t length,
                             const char *const *members, size_t member_count,
                             uint64_t required) {
    if (member_count > 64) return -1;
    json_parser parser = {.cursor = json, .end = json + length};
    uint64_t found = 0;
    if (parser_expect(&parser, '{') != 0) return -1;
    parser_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == '}') {
        parser.cursor++;
        parser_skip_space(&parser);
        return parser.cursor == parser.end && required == 0 ? 0 : -1;
    }
    for (;;) {
        char *key = NULL;
        if (parser_string(&parser, &key) != 0 ||
            parser_expect(&parser, ':') != 0) {
            free(key);
            return -1;
        }
        size_t index = 0;
        while (index < member_count && strcmp(key, members[index]) != 0)
            index++;
        free(key);
        if (index == member_count || (found & (UINT64_C(1) << index)) ||
            parser_skip_value(&parser, 0) != 0)
            return -1;
        found |= UINT64_C(1) << index;
        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == '}') {
            parser.cursor++;
            parser_skip_space(&parser);
            return parser.cursor == parser.end &&
                   (found & required) == required ? 0 : -1;
        }
        if (parser_expect(&parser, ',') != 0) return -1;
    }
}

static int json_span_string(json_span span, char **value) {
    json_parser parser = {.cursor = span.start, .end = span.end};
    if (parser_string(&parser, value) != 0) return -1;
    parser_skip_space(&parser);
    return parser.cursor == parser.end ? 0 : -1;
}

static int json_span_u64(json_span span, uint64_t *value) {
    json_parser parser = {.cursor = span.start, .end = span.end};
    if (parser_u64(&parser, value) != 0) return -1;
    parser_skip_space(&parser);
    return parser.cursor == parser.end ? 0 : -1;
}

static char *json_member_string_dup(const char *json, size_t length,
                                    const char *name) {
    json_span span = {0};
    char *value = NULL;
    if (json_object_member(json, length, name, &span) == 1 &&
        json_span_string(span, &value) == 0)
        return value;
    free(value);
    return NULL;
}

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
} openai_sha256;

static uint32_t openai_rotr32(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32 - amount));
}

static void openai_sha256_transform(openai_sha256 *ctx,
                                    const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        const unsigned char *p = block + i * 4;
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = openai_rotr32(w[i-15], 7) ^
                      openai_rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = openai_rotr32(w[i-2], 17) ^
                      openai_rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=ctx->state[0], b=ctx->state[1], c=ctx->state[2], d=ctx->state[3];
    uint32_t e=ctx->state[4], f=ctx->state[5], g=ctx->state[6], h=ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = openai_rotr32(e,6)^openai_rotr32(e,11)^openai_rotr32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = openai_rotr32(a,2)^openai_rotr32(a,13)^openai_rotr32(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void openai_sha256_init(openai_sha256 *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    static const uint32_t initial[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    memcpy(ctx->state, initial, sizeof(initial));
}

static void openai_sha256_update(openai_sha256 *ctx,
                                 const void *data, size_t length) {
    const unsigned char *p = data;
    ctx->bytes += length;
    while (length) {
        size_t take = sizeof(ctx->block) - ctx->used;
        if (take > length) take = length;
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += take; p += take; length -= take;
        if (ctx->used == sizeof(ctx->block)) {
            openai_sha256_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void openai_sha256_final(openai_sha256 *ctx,
                                unsigned char digest[32]) {
    uint64_t bits = ctx->bytes * 8;
    unsigned char one = 0x80, zeros[64] = {0}, encoded[8];
    openai_sha256_update(ctx, &one, 1);
    size_t padding = ctx->used <= 56 ? 56 - ctx->used : 120 - ctx->used;
    openai_sha256_update(ctx, zeros, padding);
    for (int i = 0; i < 8; i++) encoded[7-i] = (unsigned char)(bits >> (8*i));
    openai_sha256_update(ctx, encoded, sizeof(encoded));
    for (int i = 0; i < 8; i++) {
        digest[i*4]=(unsigned char)(ctx->state[i]>>24);
        digest[i*4+1]=(unsigned char)(ctx->state[i]>>16);
        digest[i*4+2]=(unsigned char)(ctx->state[i]>>8);
        digest[i*4+3]=(unsigned char)ctx->state[i];
    }
}

static char *openai_base64url_encode(const unsigned char *data, size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (length > (SIZE_MAX - 1) / 4 * 3) return NULL;
    size_t output = (length / 3) * 4 + (length % 3 ? length % 3 + 1 : 0);
    char *encoded = malloc(output + 1);
    if (!encoded) return NULL;
    size_t in = 0, out = 0;
    while (in + 3 <= length) {
        uint32_t v = ((uint32_t)data[in] << 16) |
                     ((uint32_t)data[in+1] << 8) | data[in+2];
        encoded[out++]=alphabet[(v>>18)&63]; encoded[out++]=alphabet[(v>>12)&63];
        encoded[out++]=alphabet[(v>>6)&63]; encoded[out++]=alphabet[v&63];
        in += 3;
    }
    if (length - in == 1) {
        uint32_t v=(uint32_t)data[in]<<16;
        encoded[out++]=alphabet[(v>>18)&63]; encoded[out++]=alphabet[(v>>12)&63];
    } else if (length - in == 2) {
        uint32_t v=((uint32_t)data[in]<<16)|((uint32_t)data[in+1]<<8);
        encoded[out++]=alphabet[(v>>18)&63]; encoded[out++]=alphabet[(v>>12)&63];
        encoded[out++]=alphabet[(v>>6)&63];
    }
    encoded[out] = '\0';
    return encoded;
}

static int openai_b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

static char *openai_base64url_decode(const char *text, size_t length,
                                     size_t *decoded_length) {
    if (length > (SIZE_MAX - 1) / 3 * 4) return NULL;
    char *decoded = malloc(length / 4 * 3 + 4);
    if (!decoded) return NULL;
    uint32_t bits = 0; unsigned bit_count = 0; size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '=') break;
        int value = openai_b64_value((unsigned char)text[i]);
        if (value < 0) { free(decoded); return NULL; }
        bits = (bits << 6) | (unsigned)value;
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            decoded[out++] = (char)((bits >> bit_count) & 0xff);
        }
    }
    decoded[out] = '\0';
    if (decoded_length) *decoded_length = out;
    return decoded;
}

static int openai_random(unsigned char *data, size_t length) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < length) {
        ssize_t count = read(fd, data + done, length - done);
        if (count > 0) done += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else { close(fd); return -1; }
    }
    return close(fd) == 0 ? 0 : -1;
}

static void provider_secret_free(char *value) {
    if (!value) return;
    volatile unsigned char *cursor = (volatile unsigned char *)value;
    size_t length = strlen(value);
    while (length--) *cursor++ = 0;
    free(value);
}

static void openai_auth_free(openai_auth *auth) {
    if (!auth) return;
    provider_secret_free(auth->id_token);
    provider_secret_free(auth->access_token);
    provider_secret_free(auth->refresh_token);
    free(auth->account_id);
    memset(auth, 0, sizeof(*auth));
}

static char *openai_jwt_payload(const char *token, size_t *length) {
    if (!token) return NULL;
    const char *first = strchr(token, '.');
    if (!first) return NULL;
    const char *second = strchr(first + 1, '.');
    if (!second) return NULL;
    return openai_base64url_decode(first + 1, (size_t)(second - first - 1),
                                   length);
}

static void openai_auth_derive_claims(openai_auth *auth) {
    size_t length = 0;
    char *payload = openai_jwt_payload(auth->access_token, &length);
    json_span span = {0};
    if (payload && json_object_member(payload, length, "exp", &span) == 1) {
        uint64_t expiry = 0;
        if (json_span_u64(span, &expiry) == 0 && expiry <= (uint64_t)INT64_MAX)
            auth->access_expires_at = (time_t)expiry;
    }
    if (payload && json_object_member(payload, length,
            "https://api.openai.com/auth", &span) == 1) {
        char *account = json_member_string_dup(
            span.start, (size_t)(span.end - span.start),
            "chatgpt_account_id");
        if (account) { free(auth->account_id); auth->account_id = account; }
    }
    free(payload);
    if (auth->account_id) return;
    payload = openai_jwt_payload(auth->id_token, &length);
    if (payload && json_object_member(payload, length,
            "https://api.openai.com/auth", &span) == 1)
        auth->account_id = json_member_string_dup(
            span.start, (size_t)(span.end - span.start),
            "chatgpt_account_id");
    free(payload);
}

#define OPENAI_AUTH_FILE_LIMIT (1024U * 1024U)

/* The host-side provider adapter owns the credential file. These small
 * filesystem helpers keep token details out of the worker API and session
 * serializer; no provider credential is explicitly passed to the runner. */
static int provider_mkdir_p(const char *path) {
    if (!path || !path[0]) return -1;
    char *copy = bridge_strdup(path);
    if (!copy) return -1;
    int result = 0;
    for (char *cursor = copy; *cursor; cursor++) {
        if (*cursor != '/') continue;
        if (cursor == copy) continue;
        *cursor = '\0';
        struct stat st;
        if (mkdir(copy, 0700) != 0 &&
            (errno != EEXIST || stat(copy, &st) != 0 || !S_ISDIR(st.st_mode))) {
            result = -1;
            *cursor = '/';
            break;
        }
        *cursor = '/';
    }
    if (result == 0) {
        struct stat st;
        if (mkdir(copy, 0700) != 0 &&
            (errno != EEXIST || stat(copy, &st) != 0 || !S_ISDIR(st.st_mode)))
            result = -1;
    }
    free(copy);
    return result;
}

static int provider_auth_parent(const char *path) {
    char *parent = bridge_strdup(path);
    if (!parent) return -1;
    char *slash = strrchr(parent, '/');
    if (!slash) {
        strcpy(parent, ".");
    } else if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    int result = provider_mkdir_p(parent);
    free(parent);
    return result;
}

/* Return 1 when the file is absent, 0 on success, and -1 on an I/O or
 * security failure.  A bounded read prevents a malformed auth path from
 * consuming unbounded memory. */
static int provider_read_auth_file(const char *path, char **text_out,
                                   size_t *length_out) {
    *text_out = NULL;
    *length_out = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
        st.st_size < 0 || (uintmax_t)st.st_size > OPENAI_AUTH_FILE_LIMIT) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    FILE *fp = fdopen(fd, "rb");
    if (!fp) {
        close(fd);
        return -1;
    }
    size_t length = (size_t)st.st_size;
    char *text = malloc(length + 1);
    if (!text) {
        fclose(fp);
        return -1;
    }
    size_t read_count = fread(text, 1, length, fp);
    int failed = ferror(fp) || read_count != length;
    fclose(fp);
    if (failed) {
        free(text);
        return -1;
    }
    text[length] = '\0';
    *text_out = text;
    *length_out = length;
    return 0;
}

static int json_member_optional_string(const char *json, size_t length,
                                       const char *name, char **value_out) {
    *value_out = NULL;
    json_span span = {0};
    int found = json_object_member(json, length, name, &span);
    if (found <= 0) return found;
    if (json_span_string(span, value_out) != 0) {
        free(*value_out);
        *value_out = NULL;
        return -1;
    }
    return 1;
}

static int json_member_optional_bool(const char *json, size_t length,
                                     const char *name, bool *value_out) {
    json_span span = {0};
    int found = json_object_member(json, length, name, &span);
    if (found <= 0) return found;
    json_parser parser = {.cursor = span.start, .end = span.end};
    if (parser_match(&parser, "true"))
        *value_out = true;
    else if (parser_match(&parser, "false"))
        *value_out = false;
    else
        return -1;
    parser_skip_space(&parser);
    return parser.cursor == parser.end ? 1 : -1;
}

static int json_member_optional_u64(const char *json, size_t length,
                                    const char *name, uint64_t *value_out) {
    json_span span = {0};
    int found = json_object_member(json, length, name, &span);
    if (found <= 0) return found;
    return json_span_u64(span, value_out) == 0 ? 1 : -1;
}

static char *openai_auth_nonempty(char *value) {
    if (value && !value[0]) {
        free(value);
        return NULL;
    }
    return value;
}

static int openai_auth_parse_file(const char *json, size_t length,
                                  openai_auth *parsed) {
    static const char *const members[] = {
        "id_token", "access_token", "refresh_token", "account_id",
        "access_expires_at", "refreshed_at",
    };
    if (memchr(json, '\0', length) != NULL ||
        !json_object_text_is_valid(json) ||
        json_object_shape(json, length, members,
                          sizeof(members) / sizeof(members[0]),
                          UINT64_C(0x3f)) != 0)
        return -1;
    char *id_token = NULL;
    char *access_token = NULL;
    char *refresh_token = NULL;
    char *account_id = NULL;
    uint64_t expires = 0, refreshed = 0;
    int result = -1;
    if (json_member_optional_string(json, length, "id_token", &id_token) < 0 ||
        json_member_optional_string(json, length, "access_token",
                                    &access_token) < 0 ||
        json_member_optional_string(json, length, "refresh_token",
                                    &refresh_token) < 0 ||
        json_member_optional_string(json, length, "account_id",
                                    &account_id) < 0)
        goto done;
    int expires_found = json_member_optional_u64(
        json, length, "access_expires_at", &expires);
    int refreshed_found = json_member_optional_u64(
        json, length, "refreshed_at", &refreshed);
    if (expires_found < 0 || refreshed_found < 0 ||
        expires > (uint64_t)INT64_MAX || refreshed > (uint64_t)INT64_MAX ||
        !access_token || !access_token[0])
        goto done;
    parsed->id_token = openai_auth_nonempty(id_token);
    id_token = NULL;
    parsed->access_token = openai_auth_nonempty(access_token);
    access_token = NULL;
    parsed->refresh_token = openai_auth_nonempty(refresh_token);
    refresh_token = NULL;
    parsed->account_id = openai_auth_nonempty(account_id);
    account_id = NULL;
    parsed->access_expires_at = expires <= (uint64_t)INT64_MAX ?
                                (time_t)expires : 0;
    parsed->refreshed_at = refreshed <= (uint64_t)INT64_MAX ?
                           (time_t)refreshed : 0;
    openai_auth_derive_claims(parsed);
    result = 0;
done:
    provider_secret_free(id_token);
    provider_secret_free(access_token);
    provider_secret_free(refresh_token);
    free(account_id);
    if (result != 0) openai_auth_free(parsed);
    return result;
}

static int provider_write_auth_file(const char *path,
                                    const char *text, size_t length) {
    if (!path || !path[0] || !text || provider_auth_parent(path) != 0)
        return -1;
    int result = -1;
    size_t path_length = strlen(path);
    if (path_length > SIZE_MAX - sizeof(".tmp.XXXXXX")) return -1;
    size_t temp_length = path_length + sizeof(".tmp.XXXXXX");
    char *temp = malloc(temp_length);
    if (!temp || snprintf(temp, temp_length, "%s.tmp.XXXXXX", path) < 0)
        return -1;
    int fd = mkstemp(temp);
    if (fd < 0) {
        free(temp);
        return -1;
    }
    bool ok = fchmod(fd, S_IRUSR | S_IWUSR) == 0;
    FILE *fp = ok ? fdopen(fd, "wb") : NULL;
    if (!fp) {
        close(fd);
        unlink(temp);
        free(temp);
        return -1;
    }
    ok = fwrite(text, 1, length, fp) == length &&
         fflush(fp) == 0 && fsync(fd) == 0;
    if (fclose(fp) != 0) ok = false;
    if (ok && rename(temp, path) == 0 && chmod(path, S_IRUSR | S_IWUSR) == 0)
        result = 0;
    if (result != 0) unlink(temp);
    free(temp);
    return result;
}

static void auth_store_error(char *error, size_t error_len,
                             const char *format, ...) {
    if (!error || !error_len) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_len, format, arguments);
    va_end(arguments);
}

typedef struct {
    const sirio_model_info *model;
    char *alias;
    sirio_reasoning_effort last_effort;
    bool active;
} sirio_model_entry;

struct sirio_model_store {
    sirio_model_entry *entries;
    size_t count;
    size_t capacity;
    sirio_provider last_provider;
    const sirio_model_info *last_models[SIRIO_PROVIDER_COUNT];
};

static bool model_alias_valid(const char *alias) {
    if (!alias || !alias[0] || strlen(alias) > 63) return false;
    for (const unsigned char *p = (const unsigned char *)alias; *p; p++)
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
            return false;
    return true;
}

static sirio_model_entry *model_store_entry(
        const sirio_model_store *store, const sirio_model_info *model) {
    if (!store || !model) return NULL;
    for (size_t i = 0; i < store->count; i++)
        if (store->entries[i].model == model)
            return &((sirio_model_store *)store)->entries[i];
    return NULL;
}

static int model_store_add(sirio_model_store *store,
                           const sirio_model_info *model,
                           const char *alias,
                           sirio_reasoning_effort effort,
                           bool active) {
    if (!store || !model || !model_alias_valid(alias) ||
        !sirio_model_supports_reasoning(model, effort) ||
        model_store_entry(store, model))
        return -1;
    for (size_t i = 0; i < store->count; i++) {
        const sirio_model_entry *entry = &store->entries[i];
        if (entry->model->provider != model->provider) continue;
        if (!strcmp(entry->alias, alias) ||
            !strcmp(entry->model->name, alias) ||
            !strcmp(entry->alias, model->name))
            return -1;
    }
    if (store->count == store->capacity) {
        size_t capacity = store->capacity ? store->capacity * 2 : 8;
        sirio_model_entry *entries = realloc(
            store->entries, capacity * sizeof(*entries));
        if (!entries) return -1;
        store->entries = entries;
        store->capacity = capacity;
    }
    char *alias_copy = bridge_strdup(alias);
    if (!alias_copy) return -1;
    store->entries[store->count++] = (sirio_model_entry){
        .model = model,
        .alias = alias_copy,
        .last_effort = effort,
        .active = active,
    };
    return 0;
}

static int model_store_add_provider_defaults(sirio_model_store *store,
                                             sirio_provider provider) {
    for (size_t i = 0; i < sirio_model_count(); i++) {
        const sirio_model_info *model = sirio_model_at(i);
        if (model->provider != provider) continue;
        if (model_store_add(store, model, model->default_alias,
                            model->default_reasoning, true) != 0)
            return -1;
    }
    return 0;
}

static int model_store_parse_provider_array(sirio_model_store *store,
                                            sirio_provider provider,
                                            json_span array,
                                            bool *dirty) {
    json_parser parser = {.cursor = array.start, .end = array.end};
    if (parser_expect(&parser, '[') != 0) return -1;
    parser_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == ']') {
        parser.cursor++;
        parser_skip_space(&parser);
        return parser.cursor == parser.end ? 0 : -1;
    }
    for (;;) {
        parser_skip_space(&parser);
        const char *start = parser.cursor;
        if (parser_skip_value(&parser, 0) != 0 || start >= parser.cursor ||
            *start != '{')
            return -1;
        size_t length = (size_t)(parser.cursor - start);
        static const char *const members[] = {
            "id", "alias", "last_effort", "active",
        };
        if (json_object_shape(start, length, members, 4,
                              UINT64_C(0x7)) != 0)
            return -1;
        char *id = NULL;
        char *alias = NULL;
        char *last_effort = NULL;
        bool active = true;
        int id_found = json_member_optional_string(start, length, "id", &id);
        int alias_found = json_member_optional_string(
            start, length, "alias", &alias);
        int effort_found = json_member_optional_string(
            start, length, "last_effort", &last_effort);
        int active_found = json_member_optional_bool(
            start, length, "active", &active);
        const sirio_model_info *model = id_found == 1 ?
            sirio_model_find_for_provider(provider, id) : NULL;
        sirio_reasoning_effort effort = model ? model->default_reasoning :
                                                SIRIO_REASONING_NONE;
        bool valid = id_found == 1 && alias_found == 1 &&
                     effort_found == 1 && model &&
                     model_alias_valid(alias) &&
                     sirio_reasoning_parse(last_effort, &effort) &&
                     sirio_model_supports_reasoning(model, effort) &&
                     active_found >= 0 &&
                     model_store_add(store, model, alias, effort,
                                     active) == 0;
        free(id);
        free(alias);
        free(last_effort);
        if (!valid || id_found < 0 || alias_found < 0 || effort_found < 0)
            return -1;
        if (active_found == 0) *dirty = true;

        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == ']') {
            parser.cursor++;
            parser_skip_space(&parser);
            return parser.cursor == parser.end ? 0 : -1;
        }
        if (parser_expect(&parser, ',') != 0) return -1;
    }
}

static int model_store_parse(sirio_model_store *store,
                             const char *json, size_t length,
                             bool *dirty) {
    if (!store || !json || memchr(json, '\0', length) ||
        !json_object_text_is_valid(json))
        return -1;
    const char *members[SIRIO_PROVIDER_COUNT + 1];
    size_t provider_count = sirio_provider_count();
    for (size_t i = 0; i < provider_count; i++)
        members[i] = sirio_provider_at(i)->name;
    members[provider_count] = "last_used";
    uint64_t required = UINT64_C(1) << provider_count;
    if (json_object_shape(json, length, members,
                          provider_count + 1, required) != 0)
        return -1;
    for (size_t i = 0; i < sirio_provider_count(); i++) {
        const sirio_provider_info *provider = sirio_provider_at(i);
        json_span array = {0};
        int found = json_object_member(json, length, provider->name, &array);
        if (found < 0 || (found == 1 && model_store_parse_provider_array(
                store, provider->id, array, dirty) != 0))
            return -1;
        if (found == 0) *dirty = true;
    }
    for (size_t i = 0; i < sirio_model_count(); i++) {
        const sirio_model_info *model = sirio_model_at(i);
        if (model_store_entry(store, model)) continue;
        if (model_store_add(store, model, model->default_alias,
                            model->default_reasoning, true) != 0)
            return -1;
        *dirty = true;
    }

    json_span last = {0};
    int last_found = json_object_member(json, length, "last_used", &last);
    if (last_found != 1) return -1;
    size_t last_length = (size_t)(last.end - last.start);
    const char *last_members[SIRIO_PROVIDER_COUNT + 1];
    last_members[0] = "provider";
    for (size_t i = 0; i < provider_count; i++)
        last_members[i + 1] = sirio_provider_at(i)->name;
    if (json_object_shape(last.start, last_length, last_members,
                          provider_count + 1, 0) != 0)
        return -1;

    for (size_t i = 0; i < provider_count; i++) {
        const sirio_provider_info *provider = sirio_provider_at(i);
        char *model_name = NULL;
        int found = json_member_optional_string(
            last.start, last_length, provider->name, &model_name);
        if (found < 0) return -1;
        if (found == 1) {
            const sirio_model_info *model = sirio_model_find_for_provider(
                provider->id, model_name);
            sirio_model_entry *entry = model_store_entry(store, model);
            bool valid = entry && entry->active;
            free(model_name);
            if (!valid) return -1;
            store->last_models[provider->id] = model;
        }
    }

    char *provider_name = NULL;
    int provider_found = json_member_optional_string(
        last.start, last_length, "provider", &provider_name);
    if (provider_found < 0) return -1;
    if (provider_found == 1) {
        const sirio_provider_info *provider = sirio_provider_find(provider_name);
        free(provider_name);
        if (!provider || !store->last_models[provider->id]) return -1;
        store->last_provider = provider->id;
    }
    return 0;
}

sirio_model_store *sirio_model_store_load(const char *path,
                                          char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    if (!path || !path[0]) {
        auth_store_error(error, error_len, "models path is not configured");
        return NULL;
    }
    sirio_model_store *store = calloc(1, sizeof(*store));
    if (!store) {
        auth_store_error(error, error_len, "out of memory");
        return NULL;
    }
    char *text = NULL;
    size_t length = 0;
    int read_result = provider_read_auth_file(path, &text, &length);
    if (read_result == 1) {
        for (size_t i = 0; i < sirio_provider_count(); i++)
            if (model_store_add_provider_defaults(
                    store, sirio_provider_at(i)->id) != 0) {
                sirio_model_store_destroy(store);
                auth_store_error(error, error_len, "out of memory");
                return NULL;
            }
        return store;
    }
    if (read_result != 0) {
        auth_store_error(error, error_len, "cannot read models file %s: %s",
                         path, strerror(errno));
        sirio_model_store_destroy(store);
        return NULL;
    }
    bool dirty = false;
    if (model_store_parse(store, text, length, &dirty) != 0) {
        auth_store_error(error, error_len, "invalid models file %s", path);
        free(text);
        sirio_model_store_destroy(store);
        return NULL;
    }
    free(text);
    if (dirty && sirio_model_store_save(
            store, path, error, error_len) != 0) {
        sirio_model_store_destroy(store);
        return NULL;
    }
    return store;
}

static int model_store_append_entry(bridge_buffer *body,
                                    const sirio_model_entry *entry) {
    return buffer_puts(body, "    {\"id\": ") == 0 &&
           json_append_string(body, entry->model->name) == 0 &&
           buffer_puts(body, ", \"alias\": ") == 0 &&
           json_append_string(body, entry->alias) == 0 &&
           buffer_puts(body, ", \"last_effort\": ") == 0 &&
           json_append_string(body,
                              sirio_reasoning_name(entry->last_effort)) == 0 &&
           buffer_puts(body, ", \"active\": ") == 0 &&
           buffer_puts(body, entry->active ? "true" : "false") == 0 &&
           buffer_putc(body, '}') == 0 ? 0 : -1;
}

int sirio_model_store_save(const sirio_model_store *store, const char *path,
                           char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    if (!store || !path || !path[0]) {
        auth_store_error(error, error_len, "models path is not configured");
        return -1;
    }
    bridge_buffer body = {0};
    bool first_provider = true;
    if (buffer_puts(&body, "{\n") != 0) goto oom;
    for (size_t p = 0; p < sirio_provider_count(); p++) {
        const sirio_provider_info *provider = sirio_provider_at(p);
        if (!first_provider && buffer_puts(&body, ",\n") != 0) goto oom;
        first_provider = false;
        if (buffer_puts(&body, "  ") != 0 ||
            json_append_string(&body, provider->name) != 0 ||
            buffer_puts(&body, ": [") != 0)
            goto oom;
        bool first_entry = true;
        for (size_t i = 0; i < store->count; i++) {
            const sirio_model_entry *entry = &store->entries[i];
            if (entry->model->provider != provider->id) continue;
            if (buffer_puts(&body, first_entry ? "\n" : ",\n") != 0 ||
                model_store_append_entry(&body, entry) != 0)
                goto oom;
            first_entry = false;
        }
        if (!first_entry && buffer_puts(&body, "\n  ") != 0) goto oom;
        if (buffer_putc(&body, ']') != 0) goto oom;
    }
    if (buffer_puts(&body, ",\n  \"last_used\": {") != 0) goto oom;
    bool first_last = true;
    if (store->last_provider != SIRIO_PROVIDER_NONE) {
        if (buffer_puts(&body, "\"provider\": ") != 0 ||
            json_append_string(&body,
                sirio_provider_name(store->last_provider)) != 0)
            goto oom;
        first_last = false;
    }
    for (size_t p = 0; p < sirio_provider_count(); p++) {
        const sirio_provider_info *provider = sirio_provider_at(p);
        const sirio_model_info *model = store->last_models[provider->id];
        if (!model) continue;
        if ((!first_last && buffer_puts(&body, ", ") != 0) ||
            json_append_string(&body, provider->name) != 0 ||
            buffer_puts(&body, ": ") != 0 ||
            json_append_string(&body, model->name) != 0)
            goto oom;
        first_last = false;
    }
    if (buffer_putc(&body, '}') != 0) goto oom;
    if (buffer_puts(&body, "\n}\n") != 0) goto oom;
    if (provider_write_auth_file(path, body.data, body.length) != 0) {
        auth_store_error(error, error_len, "cannot save models file %s: %s",
                         path, strerror(errno));
        buffer_free(&body);
        return -1;
    }
    buffer_free(&body);
    return 0;

oom:
    buffer_free(&body);
    auth_store_error(error, error_len, "out of memory");
    return -1;
}

void sirio_model_store_destroy(sirio_model_store *store) {
    if (!store) return;
    for (size_t i = 0; i < store->count; i++) free(store->entries[i].alias);
    free(store->entries);
    free(store);
}

size_t sirio_model_store_count(const sirio_model_store *store,
                               sirio_provider provider) {
    if (!store || !sirio_provider_get(provider)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < store->count; i++)
        if (store->entries[i].model->provider == provider) count++;
    return count;
}

const sirio_model_info *sirio_model_store_at(const sirio_model_store *store,
                                             sirio_provider provider,
                                             size_t index,
                                             const char **alias_out,
                                             sirio_reasoning_effort *effort_out,
                                             bool *active_out) {
    if (!store) return NULL;
    for (size_t i = 0; i < store->count; i++) {
        const sirio_model_entry *entry = &store->entries[i];
        if (entry->model->provider != provider) continue;
        if (index--) continue;
        if (alias_out) *alias_out = entry->alias;
        if (effort_out) *effort_out = entry->last_effort;
        if (active_out) *active_out = entry->active;
        return entry->model;
    }
    return NULL;
}

const sirio_model_info *sirio_model_store_first_active(
        const sirio_model_store *store, sirio_provider provider,
        const char **alias_out, sirio_reasoning_effort *effort_out) {
    if (!store) return NULL;
    for (size_t i = 0; i < store->count; i++) {
        const sirio_model_entry *entry = &store->entries[i];
        if (entry->model->provider != provider || !entry->active) continue;
        if (alias_out) *alias_out = entry->alias;
        if (effort_out) *effort_out = entry->last_effort;
        return entry->model;
    }
    return NULL;
}

const sirio_model_info *sirio_model_store_resolve(
        const sirio_model_store *store, sirio_provider provider_hint,
        const char *name, const char **alias_out,
        char *error, size_t error_len) {
    if (alias_out) *alias_out = NULL;
    if (!store || !name || !name[0]) {
        auth_store_error(error, error_len, "model name is empty");
        return NULL;
    }
    const char *model_name = name;
    char provider_name[32] = {0};
    const char *slash = strchr(name, '/');
    if (slash) {
        size_t length = (size_t)(slash - name);
        if (!length || length >= sizeof(provider_name) || !slash[1] ||
            strchr(slash + 1, '/')) {
            auth_store_error(error, error_len,
                             "model must use provider/name syntax");
            return NULL;
        }
        memcpy(provider_name, name, length);
        const sirio_provider_info *provider = sirio_provider_find(provider_name);
        if (!provider || (provider_hint != SIRIO_PROVIDER_NONE &&
                          provider_hint != provider->id)) {
            auth_store_error(error, error_len,
                             "model provider conflicts with selected provider");
            return NULL;
        }
        provider_hint = provider->id;
        model_name = slash + 1;
    }
    const sirio_model_entry *match = NULL;
    for (size_t i = 0; i < store->count; i++) {
        const sirio_model_entry *entry = &store->entries[i];
        if (provider_hint != SIRIO_PROVIDER_NONE &&
            entry->model->provider != provider_hint)
            continue;
        if (strcmp(model_name, entry->model->name) &&
            strcmp(model_name, entry->alias))
            continue;
        if (match && match->model != entry->model) {
            auth_store_error(error, error_len,
                             "model name is ambiguous; use provider/name");
            return NULL;
        }
        match = entry;
    }
    if (!match) {
        if (provider_hint != SIRIO_PROVIDER_NONE) {
            for (size_t i = 0; i < store->count; i++) {
                const sirio_model_entry *entry = &store->entries[i];
                if (entry->model->provider == provider_hint) continue;
                if (!strcmp(model_name, entry->model->name) ||
                    !strcmp(model_name, entry->alias)) {
                    auth_store_error(
                        error, error_len,
                        "model provider conflicts with selected provider");
                    return NULL;
                }
            }
        }
        auth_store_error(error, error_len, "model is not configured: %s", name);
        return NULL;
    }
    if (!match->active) {
        auth_store_error(error, error_len, "model is inactive: %s", name);
        return NULL;
    }
    if (alias_out) *alias_out = match->alias;
    return match->model;
}

const char *sirio_model_store_alias(const sirio_model_store *store,
                                    const sirio_model_info *model) {
    sirio_model_entry *entry = model_store_entry(store, model);
    return entry ? entry->alias : NULL;
}

sirio_reasoning_effort sirio_model_store_effort(
        const sirio_model_store *store, const sirio_model_info *model) {
    sirio_model_entry *entry = model_store_entry(store, model);
    return entry ? entry->last_effort :
           model ? model->default_reasoning : SIRIO_REASONING_NONE;
}

bool sirio_model_store_last_used(const sirio_model_store *store,
                                 sirio_provider *provider_out,
                                 const sirio_model_info **model_out) {
    if (!store || store->last_provider == SIRIO_PROVIDER_NONE ||
        !store->last_models[store->last_provider])
        return false;
    sirio_model_entry *entry = model_store_entry(
        store, store->last_models[store->last_provider]);
    if (!entry || !entry->active) return false;
    if (provider_out) *provider_out = store->last_provider;
    if (model_out) *model_out = store->last_models[store->last_provider];
    return true;
}

bool sirio_model_store_last_used_for_provider(
        const sirio_model_store *store, sirio_provider provider,
        const sirio_model_info **model_out) {
    if (!store || !sirio_provider_get(provider) ||
        !store->last_models[provider])
        return false;
    sirio_model_entry *entry = model_store_entry(
        store, store->last_models[provider]);
    if (!entry || !entry->active) return false;
    if (model_out) *model_out = store->last_models[provider];
    return true;
}

int sirio_model_store_set_last_used(sirio_model_store *store,
                                    const sirio_model_info *model,
                                    sirio_reasoning_effort effort) {
    sirio_model_entry *entry = model_store_entry(store, model);
    if (!entry || !entry->active ||
        !sirio_model_supports_reasoning(model, effort))
        return -1;
    entry->last_effort = effort;
    store->last_models[model->provider] = model;
    store->last_provider = model->provider;
    return 0;
}

static int openai_auth_copy(openai_auth *destination,
                            const openai_auth *source) {
    openai_auth copy = {0};
    if (source && source->id_token)
        copy.id_token = bridge_strdup(source->id_token);
    if (source && source->access_token)
        copy.access_token = bridge_strdup(source->access_token);
    if (source && source->refresh_token)
        copy.refresh_token = bridge_strdup(source->refresh_token);
    if (source && source->account_id)
        copy.account_id = bridge_strdup(source->account_id);
    if (source && ((source->id_token && !copy.id_token) ||
                   (source->access_token && !copy.access_token) ||
                   (source->refresh_token && !copy.refresh_token) ||
                   (source->account_id && !copy.account_id))) {
        openai_auth_free(&copy);
        return -1;
    }
    copy.access_expires_at = source ? source->access_expires_at : 0;
    copy.refreshed_at = source ? source->refreshed_at : 0;
    openai_auth_free(destination);
    *destination = copy;
    return 0;
}

static char *json_span_dup(json_span span) {
    size_t length = (size_t)(span.end - span.start);
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, span.start, length);
    copy[length] = '\0';
    return copy;
}

static int auth_store_parse_provider(sirio_auth_store *store,
                                     sirio_provider provider,
                                     json_span object) {
    size_t length = (size_t)(object.end - object.start);
    static const char *const common_members[] = {
        "auth_method", "api_key",
    };
    static const char *const openai_members[] = {
        "auth_method", "api_key", "oauth",
    };
    const char *const *members = provider == SIRIO_PROVIDER_OPENAI ?
                                 openai_members : common_members;
    size_t member_count = provider == SIRIO_PROVIDER_OPENAI ? 3 : 2;
    if (json_object_shape(object.start, length, members, member_count, 0) != 0)
        return -1;
    char *api_key = NULL;
    char *method = NULL;
    int key_found = json_member_optional_string(
        object.start, length, "api_key", &api_key);
    int method_found = json_member_optional_string(
        object.start, length, "auth_method", &method);
    if (key_found < 0 || method_found < 0 ||
        (key_found == 1 && (!api_key || !api_key[0])))
        goto fail;
    if (key_found == 1) {
        store->api_keys[provider] = api_key;
        api_key = NULL;
    }

    if (provider == SIRIO_PROVIDER_OPENAI) {
        json_span oauth = {0};
        int oauth_found = json_object_member(
            object.start, length, "oauth", &oauth);
        if (oauth_found < 0) goto fail;
        if (oauth_found == 1) {
            char *oauth_json = json_span_dup(oauth);
            if (!oauth_json || openai_auth_parse_file(
                    oauth_json, strlen(oauth_json), &store->openai) != 0) {
                free(oauth_json);
                goto fail;
            }
            free(oauth_json);
        }
    }

    if (method_found == 1) {
        if (!strcmp(method, "api-key"))
            store->preferred[provider] = SIRIO_AUTH_API_KEY;
        else if (!strcmp(method, "oauth"))
            store->preferred[provider] = SIRIO_AUTH_OAUTH;
        else
            goto fail;
    }
    free(method);
    if (store->preferred[provider] != SIRIO_AUTH_NONE &&
        !sirio_auth_store_has(store, provider, store->preferred[provider]))
        return -1;
    return 0;

fail:
    free(api_key);
    free(method);
    return -1;
}

static int auth_store_parse(sirio_auth_store *store,
                            const char *json, size_t length) {
    if (!store || !json || memchr(json, '\0', length) != NULL ||
        !json_object_text_is_valid(json))
        return -1;

    static const char *const top_members[] = {"providers"};
    if (json_object_shape(json, length, top_members, 1, UINT64_C(1)) != 0)
        return -1;

    json_span providers = {0};
    int providers_found = json_object_member(
        json, length, "providers", &providers);
    if (providers_found != 1) return -1;
    size_t providers_length = (size_t)(providers.end - providers.start);
    const char *members[SIRIO_PROVIDER_COUNT];
    size_t provider_count = sirio_provider_count();
    for (size_t i = 0; i < provider_count; i++)
        members[i] = sirio_provider_at(i)->name;
    if (json_object_shape(providers.start, providers_length, members,
                          provider_count, 0) != 0)
        return -1;
    for (size_t i = 0; i < sirio_provider_count(); i++) {
        const sirio_provider_info *info = sirio_provider_at(i);
        json_span object = {0};
        int found = json_object_member(providers.start, providers_length,
                                       info->name, &object);
        if (found < 0 ||
            (found == 1 &&
             auth_store_parse_provider(store, info->id, object) != 0))
            return -1;
    }
    return 0;
}

static int auth_store_append_oauth(bridge_buffer *body,
                                   const openai_auth *auth) {
    char number[32];
    if (buffer_puts(body, "{\"id_token\":") != 0 ||
        json_append_string(body, auth->id_token) != 0 ||
        buffer_puts(body, ",\"access_token\":") != 0 ||
        json_append_string(body, auth->access_token) != 0 ||
        buffer_puts(body, ",\"refresh_token\":") != 0 ||
        json_append_string(body, auth->refresh_token) != 0 ||
        buffer_puts(body, ",\"account_id\":") != 0 ||
        json_append_string(body, auth->account_id) != 0 ||
        buffer_puts(body, ",\"access_expires_at\":") != 0)
        return -1;
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)(uint64_t)(auth->access_expires_at > 0 ?
                                             auth->access_expires_at : 0));
    if (buffer_puts(body, number) != 0 ||
        buffer_puts(body, ",\"refreshed_at\":") != 0)
        return -1;
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)(uint64_t)(auth->refreshed_at > 0 ?
                                             auth->refreshed_at : 0));
    return buffer_puts(body, number) == 0 && buffer_putc(body, '}') == 0 ?
           0 : -1;
}

static int auth_store_build_json(const sirio_auth_store *store,
                                 bridge_buffer *body) {
    if (buffer_puts(body, "{\"providers\":{") != 0) return -1;
    bool first_provider = true;
    for (size_t i = 0; i < sirio_provider_count(); i++) {
        const sirio_provider_info *info = sirio_provider_at(i);
        bool configured = store->api_keys[info->id] ||
            (info->id == SIRIO_PROVIDER_OPENAI &&
             store->openai.access_token);
        if (!configured) continue;
        if (!first_provider && buffer_putc(body, ',') != 0) return -1;
        first_provider = false;
        if (json_append_string(body, info->name) != 0 ||
            buffer_puts(body, ":{") != 0)
            return -1;
        bool field = false;
        sirio_auth_method preferred = store->preferred[info->id];
        if (preferred != SIRIO_AUTH_NONE) {
            if (buffer_puts(body, "\"auth_method\":") != 0 ||
                json_append_string(body, preferred == SIRIO_AUTH_API_KEY ?
                                   "api-key" : "oauth") != 0)
                return -1;
            field = true;
        }
        if (store->api_keys[info->id]) {
            if ((field && buffer_putc(body, ',') != 0) ||
                buffer_puts(body, "\"api_key\":") != 0 ||
                json_append_string(body, store->api_keys[info->id]) != 0)
                return -1;
            field = true;
        }
        if (info->id == SIRIO_PROVIDER_OPENAI &&
            store->openai.access_token) {
            if ((field && buffer_putc(body, ',') != 0) ||
                buffer_puts(body, "\"oauth\":") != 0 ||
                auth_store_append_oauth(body, &store->openai) != 0)
                return -1;
        }
        if (buffer_putc(body, '}') != 0) return -1;
    }
    return buffer_puts(body, "}}\n");
}

sirio_auth_store *sirio_auth_store_load(const char *path,
                                        char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    if (!path || !path[0]) {
        auth_store_error(error, error_len, "auth path is not configured");
        return NULL;
    }
    sirio_auth_store *store = calloc(1, sizeof(*store));
    if (!store) {
        auth_store_error(error, error_len, "out of memory");
        return NULL;
    }
    char *text = NULL;
    size_t length = 0;
    int read_result = provider_read_auth_file(path, &text, &length);
    if (read_result == 1) return store;
    if (read_result != 0) {
        auth_store_error(error, error_len, "cannot read auth file %s: %s",
                         path, strerror(errno));
        sirio_auth_store_destroy(store);
        return NULL;
    }
    if (auth_store_parse(store, text, length) != 0) {
        auth_store_error(error, error_len, "invalid auth file %s", path);
        free(text);
        sirio_auth_store_destroy(store);
        return NULL;
    }
    free(text);
    return store;
}

int sirio_auth_store_save(const sirio_auth_store *store, const char *path,
                          char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    if (!store || !path || !path[0]) {
        auth_store_error(error, error_len, "auth path is not configured");
        return -1;
    }
    bridge_buffer body = {0};
    if (auth_store_build_json(store, &body) != 0) {
        buffer_free(&body);
        auth_store_error(error, error_len, "out of memory");
        return -1;
    }
    int result = provider_write_auth_file(path, body.data, body.length);
    if (result != 0)
        auth_store_error(error, error_len, "cannot save auth file %s: %s",
                         path, strerror(errno));
    buffer_free(&body);
    return result;
}

void sirio_auth_store_destroy(sirio_auth_store *store) {
    if (!store) return;
    for (size_t i = 0; i < SIRIO_PROVIDER_COUNT; i++)
        provider_secret_free(store->api_keys[i]);
    openai_auth_free(&store->openai);
    free(store);
}

bool sirio_auth_store_has(const sirio_auth_store *store,
                          sirio_provider provider,
                          sirio_auth_method method) {
    if (!store || !sirio_provider_get(provider)) return false;
    if (method == SIRIO_AUTH_API_KEY)
        return store->api_keys[provider] && store->api_keys[provider][0];
    if (method == SIRIO_AUTH_OAUTH)
        return provider == SIRIO_PROVIDER_OPENAI &&
               store->openai.access_token && store->openai.access_token[0];
    return false;
}

sirio_auth_method sirio_auth_store_preferred(
        const sirio_auth_store *store, sirio_provider provider) {
    return store && sirio_provider_get(provider) ?
           store->preferred[provider] : SIRIO_AUTH_NONE;
}

const char *sirio_auth_store_api_key(const sirio_auth_store *store,
                                     sirio_provider provider) {
    return store && sirio_provider_get(provider) ?
           store->api_keys[provider] : NULL;
}

time_t sirio_auth_store_oauth_expiry(const sirio_auth_store *store,
                                     sirio_provider provider) {
    return store && provider == SIRIO_PROVIDER_OPENAI ?
           store->openai.access_expires_at : 0;
}

int sirio_auth_store_set_api_key(sirio_auth_store *store,
                                 sirio_provider provider,
                                 const char *api_key) {
    const sirio_provider_info *info = sirio_provider_get(provider);
    if (!store || !info || !info->supports_api_key || !api_key || !api_key[0])
        return -1;
    char *copy = bridge_strdup(api_key);
    if (!copy) return -1;
    provider_secret_free(store->api_keys[provider]);
    store->api_keys[provider] = copy;
    return 0;
}

int sirio_auth_store_set_preferred(sirio_auth_store *store,
                                   sirio_provider provider,
                                   sirio_auth_method method) {
    const sirio_provider_info *info = sirio_provider_get(provider);
    if (!store || !info || method < SIRIO_AUTH_NONE ||
        method > SIRIO_AUTH_OAUTH)
        return -1;
    if ((method == SIRIO_AUTH_API_KEY && !info->supports_api_key) ||
        (method == SIRIO_AUTH_OAUTH && !info->supports_oauth) ||
        (method != SIRIO_AUTH_NONE &&
         !sirio_auth_store_has(store, provider, method)))
        return -1;
    store->preferred[provider] = method;
    return 0;
}

void sirio_auth_store_clear_provider(sirio_auth_store *store,
                                     sirio_provider provider) {
    if (!store || !sirio_provider_get(provider)) return;
    provider_secret_free(store->api_keys[provider]);
    store->api_keys[provider] = NULL;
    store->preferred[provider] = SIRIO_AUTH_NONE;
    if (provider == SIRIO_PROVIDER_OPENAI) openai_auth_free(&store->openai);
}

typedef struct {
    bridge_buffer body;
    int failed;
} openai_http_body;

static size_t openai_http_body_write(char *data, size_t size, size_t count,
                                     void *private_data) {
    openai_http_body *body = private_data;
    if (size && count > SIZE_MAX / size) { body->failed = 1; return 0; }
    size_t length = size * count;
    if (length > OPENAI_RESPONSE_LIMIT - body->body.length ||
        buffer_append(&body->body, data, length) != 0) {
        body->failed = 1;
        return 0;
    }
    return length;
}

static int openai_token_request(sirio_bridge *bridge, const char *body,
                                const char *content_type,
                                openai_auth *tokens) {
    CURL *curl = curl_easy_init();
    if (!curl) { bridge_set_error(bridge, "unable to create OAuth request"); return -1; }
    struct curl_slist *headers = NULL;
    char content_header[96];
    snprintf(content_header, sizeof(content_header), "Content-Type: %s", content_type);
    headers = curl_slist_append(headers, content_header);
    openai_http_body response = {0};
    char curl_error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, OPENAI_TOKEN_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, openai_http_body_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    CURLcode status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (status != CURLE_OK || response.failed) {
        bridge_set_error(bridge, "OpenAI OAuth transport error: %s",
                         curl_error[0] ? curl_error : curl_easy_strerror(status));
        buffer_free(&response.body);
        return -1;
    }
    const char *json = response.body.data ? response.body.data : "";
    size_t length = response.body.length;
    if (http_status < 200 || http_status >= 300) {
        char *description = json_member_string_dup(json, length, "error_description");
        if (!description) description = json_member_string_dup(json, length, "error");
        bridge_set_error(bridge, "OpenAI OAuth HTTP %ld: %s", http_status,
                         description ? description : "token request failed");
        free(description);
        buffer_free(&response.body);
        return -1;
    }
    openai_auth parsed = {0};
    parsed.id_token = json_member_string_dup(json, length, "id_token");
    parsed.access_token = json_member_string_dup(json, length, "access_token");
    parsed.refresh_token = json_member_string_dup(json, length, "refresh_token");
    if (!parsed.access_token) {
        bridge_set_error(bridge, "OpenAI OAuth response has no access token");
        openai_auth_free(&parsed);
        buffer_free(&response.body);
        return -1;
    }
    openai_auth_derive_claims(&parsed);
    parsed.refreshed_at = time(NULL);
    *tokens = parsed;
    buffer_free(&response.body);
    return 0;
}

static int openai_auth_refresh(sirio_bridge *bridge) {
    if (!bridge->openai.refresh_token || !bridge->openai.refresh_token[0]) {
        bridge_set_error(bridge, "OpenAI refresh token is unavailable");
        return -1;
    }
    bridge_buffer request = {0};
    if (buffer_puts(&request, "{\"client_id\":") != 0 ||
        json_append_string(&request, OPENAI_CLIENT_ID) != 0 ||
        buffer_puts(&request, ",\"grant_type\":\"refresh_token\",\"refresh_token\":") != 0 ||
        json_append_string(&request, bridge->openai.refresh_token) != 0 ||
        buffer_putc(&request, '}') != 0) {
        buffer_free(&request); bridge_set_error(bridge, "out of memory"); return -1;
    }
    openai_auth refreshed = {0};
    int result = openai_token_request(bridge, request.data,
                                      "application/json", &refreshed);
    buffer_free(&request);
    if (result != 0) return -1;
    if (!refreshed.refresh_token)
        refreshed.refresh_token = bridge_strdup(bridge->openai.refresh_token);
    if (!refreshed.id_token && bridge->openai.id_token)
        refreshed.id_token = bridge_strdup(bridge->openai.id_token);
    if (!refreshed.account_id && bridge->openai.account_id)
        refreshed.account_id = bridge_strdup(bridge->openai.account_id);
    openai_auth_free(&bridge->openai);
    bridge->openai = refreshed;
    if (bridge->auth_path && bridge->auth_path[0] &&
        sirio_bridge_save_auth(bridge, bridge->auth_path) != 0)
        return -1;
    return 0;
}

static char *openai_query_value(CURL *curl, const char *target,
                                const char *name) {
    const char *query = strchr(target, '?');
    if (!query) return NULL;
    query++;
    size_t name_len = strlen(name);
    while (*query) {
        const char *end = strchr(query, '&');
        if (!end) end = query + strlen(query);
        const char *equals = memchr(query, '=', (size_t)(end - query));
        if (equals && (size_t)(equals - query) == name_len &&
            !memcmp(query, name, name_len)) {
            int decoded_len = 0;
            char *decoded = curl_easy_unescape(curl, equals + 1,
                (int)(end - equals - 1), &decoded_len);
            if (!decoded) return NULL;
            char *copy = malloc((size_t)decoded_len + 1);
            if (copy) { memcpy(copy, decoded, (size_t)decoded_len); copy[decoded_len]='\0'; }
            curl_free(decoded);
            return copy;
        }
        query = *end ? end + 1 : end;
    }
    return NULL;
}

static int openai_callback_listener(int *port_out) {
    static const int ports[] = {1455, 1457};
    for (size_t i = 0; i < sizeof(ports)/sizeof(ports[0]); i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((uint16_t)ports[i]);
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
            listen(fd, 1) == 0) {
            *port_out = ports[i];
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int openai_accept_client(int listener) {
    int client = accept(listener, NULL, NULL);
    if (client >= 0) (void)fcntl(client, F_SETFD, FD_CLOEXEC);
    return client;
}

static ssize_t openai_send_response(int fd, const void *data, size_t length) {
#ifdef MSG_NOSIGNAL
    return send(fd, data, length, MSG_NOSIGNAL);
#else
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                     &enabled, sizeof(enabled));
#endif
    return send(fd, data, length, 0);
#endif
}

static int openai_oauth_login(sirio_bridge *bridge) {
    unsigned char verifier_bytes[32], state_bytes[24], digest[32];
    if (openai_random(verifier_bytes, sizeof(verifier_bytes)) != 0 ||
        openai_random(state_bytes, sizeof(state_bytes)) != 0) {
        bridge_set_error(bridge, "unable to obtain OAuth randomness"); return -1;
    }
    char *verifier = openai_base64url_encode(verifier_bytes, sizeof(verifier_bytes));
    char *state = openai_base64url_encode(state_bytes, sizeof(state_bytes));
    if (!verifier || !state) {
        free(verifier); free(state);
        bridge_set_error(bridge, "out of memory"); return -1;
    }
    openai_sha256 sha;
    openai_sha256_init(&sha); openai_sha256_update(&sha, verifier, strlen(verifier));
    openai_sha256_final(&sha, digest);
    char *challenge = openai_base64url_encode(digest, sizeof(digest));
    if (!challenge) {
        free(verifier); free(state); free(challenge);
        bridge_set_error(bridge, "out of memory"); return -1;
    }
    int port = 0;
    int listener = openai_callback_listener(&port);
    if (listener < 0) {
        free(verifier); free(state); free(challenge);
        bridge_set_error(bridge, "cannot listen on OAuth callback ports 1455 or 1457");
        return -1;
    }
    char redirect[96];
    snprintf(redirect, sizeof(redirect), "http://localhost:%d/auth/callback", port);
    CURL *escape = curl_easy_init();
    char *escaped_redirect = escape ? curl_easy_escape(escape, redirect, 0) : NULL;
    char *escaped_scope = escape ? curl_easy_escape(escape,
        "openid profile email offline_access api.connectors.read api.connectors.invoke", 0) : NULL;
    bridge_buffer url = {0};
    int built = escaped_redirect && escaped_scope &&
        buffer_puts(&url, OPENAI_AUTHORIZE_ENDPOINT "?response_type=code&client_id=" OPENAI_CLIENT_ID "&redirect_uri=") == 0 &&
        buffer_puts(&url, escaped_redirect) == 0 &&
        buffer_puts(&url, "&scope=") == 0 && buffer_puts(&url, escaped_scope) == 0 &&
        buffer_puts(&url, "&code_challenge=") == 0 && buffer_puts(&url, challenge) == 0 &&
        buffer_puts(&url, "&code_challenge_method=S256&id_token_add_organizations=true&codex_cli_simplified_flow=true&state=") == 0 &&
        buffer_puts(&url, state) == 0 && buffer_puts(&url, "&originator=codex_cli_rs") == 0;
    curl_free(escaped_redirect); curl_free(escaped_scope);
    if (!built) {
        if (escape) curl_easy_cleanup(escape);
        close(listener);
        buffer_free(&url);
        free(verifier); free(state); free(challenge);
        bridge_set_error(bridge, "unable to build OAuth authorization URL"); return -1;
    }
    pid_t browser = fork();
    if (browser == 0) {
#ifdef __APPLE__
        execl("/usr/bin/open", "open", url.data, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", url.data, (char *)NULL);
#endif
        _exit(127);
    }
    if (browser < 0) {
        curl_easy_cleanup(escape); close(listener); buffer_free(&url);
        free(verifier); free(state); free(challenge);
        bridge_set_error(bridge, "cannot start browser: %s", strerror(errno)); return -1;
    }
    fprintf(stderr, "Open this URL to authenticate OpenAI:\n%s\n", url.data);
    struct pollfd pollfd = {.fd = listener, .events = POLLIN};
    int ready;
    do ready = poll(&pollfd, 1, 300000); while (ready < 0 && errno == EINTR);
    int client = ready > 0 ? openai_accept_client(listener) : -1;
    close(listener);
    char request[8192] = {0};
    ssize_t received = client >= 0 ? recv(client, request, sizeof(request)-1, 0) : -1;
    char *code = NULL, *returned_state = NULL, *oauth_error = NULL;
    if (received > 0 && !strncmp(request, "GET ", 4)) {
        char *space = strchr(request + 4, ' ');
        if (space) {
            *space = '\0';
            code = openai_query_value(escape, request + 4, "code");
            returned_state = openai_query_value(escape, request + 4, "state");
            oauth_error = openai_query_value(escape, request + 4, "error");
        }
    }
    bool valid = code && returned_state && !strcmp(returned_state, state) && !oauth_error;
    if (client >= 0) {
        const char *body = valid ?
            "Authentication complete. You can close this window." :
            "Authentication failed. Return to Sirio for details.";
        char response[512];
        int length = snprintf(response, sizeof(response),
            "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s",
            valid ? "200 OK" : "400 Bad Request", strlen(body), body);
        if (length > 0)
            (void)openai_send_response(
                client, response, (size_t)length);
        close(client);
    }
    waitpid(browser, NULL, WNOHANG);
    buffer_free(&url); free(challenge); free(state); free(returned_state);
    if (!valid) {
        bridge_set_error(bridge, "%s", oauth_error ? oauth_error :
                         ready == 0 ? "OpenAI OAuth callback timed out" :
                         "invalid OpenAI OAuth callback");
        free(oauth_error); free(code); free(verifier); curl_easy_cleanup(escape);
        return -1;
    }
    char *escaped_code = curl_easy_escape(escape, code, 0);
    char *escaped_verifier = curl_easy_escape(escape, verifier, 0);
    char *escaped_callback = curl_easy_escape(escape, redirect, 0);
    bridge_buffer exchange_body = {0};
    int exchange_built = escaped_code && escaped_verifier && escaped_callback &&
        buffer_puts(&exchange_body, "grant_type=authorization_code&code=") == 0 &&
        buffer_puts(&exchange_body, escaped_code) == 0 &&
        buffer_puts(&exchange_body, "&redirect_uri=") == 0 &&
        buffer_puts(&exchange_body, escaped_callback) == 0 &&
        buffer_puts(&exchange_body, "&client_id=" OPENAI_CLIENT_ID "&code_verifier=") == 0 &&
        buffer_puts(&exchange_body, escaped_verifier) == 0;
    curl_free(escaped_code); curl_free(escaped_verifier); curl_free(escaped_callback);
    curl_easy_cleanup(escape); free(oauth_error); free(code); free(verifier);
    if (!exchange_built) {
        buffer_free(&exchange_body); bridge_set_error(bridge, "out of memory"); return -1;
    }
    openai_auth tokens = {0};
    int result = openai_token_request(bridge, exchange_body.data,
        "application/x-www-form-urlencoded", &tokens);
    buffer_free(&exchange_body);
    if (result != 0) return -1;
    if (!tokens.refresh_token || !tokens.account_id) {
        openai_auth_free(&tokens);
        bridge_set_error(bridge, "OpenAI OAuth response lacks refresh token or account id");
        return -1;
    }
    openai_auth_free(&bridge->openai);
    bridge->openai = tokens;
    return 0;
}

static int openai_append_input_item(bridge_buffer *request, int *first,
                                    const char *json, size_t length) {
    if (!*first && buffer_putc(request, ',') != 0) return -1;
    *first = 0;
    return buffer_append(request, json, length);
}

static int openai_append_state_items(bridge_buffer *request, int *first,
                                     const char *json) {
    const char *start = json;
    while (isspace((unsigned char)*start)) start++;
    if (*start++ != '[') return -1;
    const char *end = json + strlen(json);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (end <= start || end[-1] != ']') return -1;
    end--;
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (start == end) return 0;
    if (!*first && buffer_putc(request, ',') != 0) return -1;
    *first = 0;
    return buffer_append(request, start, (size_t)(end - start));
}

static int openai_append_message_item(bridge_buffer *request, int *first,
                                      const char *role, const char *kind,
                                      const char *text) {
    if (!*first && buffer_putc(request, ',') != 0) return -1;
    *first = 0;
    if (buffer_puts(request, "{\"type\":\"message\",\"role\":") != 0 ||
        json_append_string(request, role) != 0 ||
        buffer_puts(request, ",\"content\":[{\"type\":") != 0 ||
        json_append_string(request, kind) != 0 ||
        buffer_puts(request, ",\"text\":") != 0 ||
        json_append_string(request, text ? text : "") != 0 ||
        buffer_puts(request, "}]}") != 0)
        return -1;
    return 0;
}

static int openai_append_tool_definitions(bridge_buffer *request,
                                          const sirio_tool *tools,
                                          size_t tool_count) {
    for (size_t i = 0; i < tool_count; i++) {
        if (i && buffer_putc(request, ',') != 0) return -1;
        if (buffer_puts(request, "{\"type\":\"function\",\"name\":") != 0 ||
            json_append_string(request, tools[i].name) != 0 ||
            buffer_puts(request, ",\"description\":") != 0 ||
            json_append_string(request, tools[i].description) != 0 ||
            buffer_puts(request, ",\"parameters\":") != 0 ||
            buffer_puts(request, tools[i].input_schema_json) != 0 ||
            buffer_puts(request, ",\"strict\":false}") != 0)
            return -1;
    }
    return 0;
}

static int openai_build_request_for_transport(
                                bridge_buffer *request,
                                const char *model,
                                bool platform_api,
                                const sirio_generation_options *generation,
                                const sirio_message *messages,
                                size_t message_count,
                                const sirio_tool *tools,
                                size_t tool_count) {
    if (!model || !model[0] || !generation ||
        !messages_are_valid(messages, message_count) ||
        !tools_are_valid(tools, tool_count))
        return -1;
    const char *instructions = "";
    size_t start = 0;
    if (message_count && messages[0].role == SIRIO_ROLE_SYSTEM) {
        instructions = messages[0].content;
        start = 1;
    }
    if (buffer_puts(request, "{\"model\":") != 0 ||
        json_append_string(request, model) != 0 ||
        buffer_puts(request, ",\"input\":[") != 0)
        return -1;
    int first = 1;
    if (tool_count && !platform_api) {
        if (buffer_puts(request,
                "{\"type\":\"additional_tools\",\"role\":\"developer\",\"tools\":[") != 0 ||
            openai_append_tool_definitions(request, tools, tool_count) != 0 ||
            buffer_puts(request, "]}") != 0)
            return -1;
        first = 0;
    }
    if (instructions[0] &&
        openai_append_message_item(request, &first, "developer",
                                   "input_text", instructions) != 0)
        return -1;
    for (size_t i = start; i < message_count; i++) {
        const sirio_message *message = &messages[i];
        if (message->role == SIRIO_ROLE_ASSISTANT &&
            message->provider_state_json &&
            (message->provider == SIRIO_PROVIDER_NONE ||
             message->provider == SIRIO_PROVIDER_OPENAI)) {
            if (openai_append_state_items(request, &first,
                                          message->provider_state_json) != 0)
                return -1;
            continue;
        }
        if (message->role == SIRIO_ROLE_USER ||
            message->role == SIRIO_ROLE_SYSTEM) {
            if (openai_append_message_item(request, &first,
                    message->role == SIRIO_ROLE_USER ? "user" : "developer",
                    "input_text", message->content) != 0)
                return -1;
        } else if (message->role == SIRIO_ROLE_ASSISTANT) {
            if (message->content && message->content[0] &&
                openai_append_message_item(request, &first, "assistant",
                                           "output_text",
                                           message->content) != 0)
                return -1;
            for (size_t j = 0; j < message->tool_call_count; j++) {
                bridge_buffer item = {0};
                const sirio_tool_call *call = &message->tool_calls[j];
                int ok = buffer_puts(&item, "{\"type\":\"function_call\",\"call_id\":") == 0 &&
                    json_append_string(&item, call->id) == 0 &&
                    buffer_puts(&item, ",\"name\":") == 0 &&
                    json_append_string(&item, call->name) == 0 &&
                    buffer_puts(&item, ",\"arguments\":") == 0 &&
                    json_append_string(&item, call->arguments_json) == 0 &&
                    buffer_putc(&item, '}') == 0;
                if (!ok || openai_append_input_item(request, &first,
                        item.data, item.length) != 0) {
                    buffer_free(&item); return -1;
                }
                buffer_free(&item);
            }
        } else {
            bridge_buffer item = {0};
            int ok = buffer_puts(&item,
                    "{\"type\":\"function_call_output\",\"call_id\":") == 0 &&
                json_append_string(&item, message->tool_call_id) == 0 &&
                buffer_puts(&item, ",\"output\":") == 0 &&
                json_append_string(&item, message->content) == 0 &&
                buffer_putc(&item, '}') == 0;
            if (!ok || openai_append_input_item(request, &first,
                    item.data, item.length) != 0) {
                buffer_free(&item); return -1;
            }
            buffer_free(&item);
        }
    }
    if (buffer_putc(request, ']') != 0) return -1;
    if (tool_count && platform_api) {
        if (buffer_puts(request, ",\"tools\":[") != 0 ||
            openai_append_tool_definitions(request, tools, tool_count) != 0 ||
            buffer_putc(request, ']') != 0)
            return -1;
    }
    if (tool_count && buffer_puts(request,
            ",\"tool_choice\":\"auto\"") != 0)
        return -1;
    /* The ChatGPT Codex transport requires this field even for raw requests
     * whose tool list is empty. The public Platform endpoint only needs it
     * when tools are actually present. */
    if ((tool_count || !platform_api) &&
        buffer_puts(request, ",\"parallel_tool_calls\":false") != 0)
            return -1;
    const char *effort = sirio_reasoning_name(generation->reasoning);
    if (!effort) return -1;
    if (buffer_puts(request, ",\"reasoning\":{\"effort\":") != 0 ||
        json_append_string(request, effort) != 0 ||
        buffer_puts(request, ",\"summary\":\"auto\"") != 0 ||
        (!platform_api && buffer_puts(request,
            ",\"context\":\"all_turns\"") != 0) ||
        buffer_puts(request, "},\"text\":{\"verbosity\":\"low\"}") != 0)
        return -1;
    if (platform_api && generation->max_tokens > 0 &&
        append_json_int(request, "max_output_tokens",
                        generation->max_tokens) != 0)
        return -1;
    if (generation->reasoning == SIRIO_REASONING_NONE) {
        if (generation->temperature >= 0.0 &&
            append_json_double(request, "temperature",
                               generation->temperature) != 0)
            return -1;
        if (generation->top_p >= 0.0 &&
            append_json_double(request, "top_p", generation->top_p) != 0)
            return -1;
    }
    return buffer_puts(request,
        ",\"include\":[\"reasoning.encrypted_content\"],\"store\":false,\"stream\":true}");
}

/* Kept for direct adapter contract tests of the ChatGPT OAuth mapping. */
#ifdef SIRIO_NO_MAIN
static int __attribute__((unused)) openai_build_request(bridge_buffer *request,
                                const sirio_generation_options *generation,
                                const sirio_message *messages,
                                size_t message_count,
                                const sirio_tool *tools,
                                size_t tool_count) {
    return openai_build_request_for_transport(
        request, OPENAI_MODEL, false, generation, messages, message_count,
        tools, tool_count);
}
#endif

typedef struct {
    sirio_bridge *bridge;
    sirio_bridge_event_callback callback;
    void *private_data;
    bridge_buffer body;
    bridge_buffer pending;
    bridge_buffer event_data;
    bridge_buffer provider_items;
    uint64_t received;
    size_t tool_call_count;
    int content_type_seen;
    int is_sse;
    int failed;
    int callback_rejected;
    int completed;
    int provider_items_started;
    int reasoning_summary_seen;
    char error[256];
    char *request_id;
    char *model;
} openai_stream;

static int openai_stream_fail(openai_stream *stream, const char *format, ...) {
    if (!stream->error[0]) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(stream->error, sizeof(stream->error), format, arguments);
        va_end(arguments);
    }
    stream->failed = 1;
    return -1;
}

static int openai_stream_emit(openai_stream *stream,
                              const sirio_bridge_event *event) {
    if (!stream->callback || stream->callback(event, stream->private_data) == 0)
        return 0;
    stream->callback_rejected = 1;
    return openai_stream_fail(stream, "agent rejected an OpenAI provider event");
}

static int openai_stream_store_item(openai_stream *stream, json_span item) {
    if (!stream->provider_items_started) {
        if (buffer_putc(&stream->provider_items, '[') != 0)
            return openai_stream_fail(stream, "out of memory");
        stream->provider_items_started = 1;
    } else if (buffer_putc(&stream->provider_items, ',') != 0) {
        return openai_stream_fail(stream, "out of memory");
    }
    if (buffer_append(&stream->provider_items, item.start,
                      (size_t)(item.end - item.start)) != 0)
        return openai_stream_fail(stream, "OpenAI continuation state exceeded limits");
    return 0;
}

/* Some Responses transports stream summary deltas. Others only place the
 * completed summary on the reasoning output item. Use that item as a
 * fallback, but never replay a summary already rendered from deltas. */
static int openai_stream_emit_item_summary(openai_stream *stream,
                                           json_span item) {
    if (stream->reasoning_summary_seen) return 0;
    json_span summary = {0};
    size_t item_length = (size_t)(item.end - item.start);
    int found = json_object_member(item.start, item_length, "summary", &summary);
    if (found < 0)
        return openai_stream_fail(stream, "OpenAI reasoning summary is malformed");
    if (found == 0) return 0;

    json_parser parser = {.cursor = summary.start, .end = summary.end};
    if (parser_expect(&parser, '[') != 0)
        return openai_stream_fail(stream, "OpenAI reasoning summary is not an array");
    parser_skip_space(&parser);
    while (parser.cursor < parser.end && *parser.cursor != ']') {
        const char *entry = parser.cursor;
        if (parser_skip_value(&parser, 0) != 0 || entry >= parser.cursor ||
            *entry != '{')
            return openai_stream_fail(stream,
                                      "OpenAI reasoning summary item is malformed");
        size_t entry_length = (size_t)(parser.cursor - entry);
        char *summary_type = json_member_string_dup(entry, entry_length, "type");
        char *text = json_member_string_dup(entry, entry_length, "text");
        if (summary_type && !strcmp(summary_type, "summary_text") && text) {
            sirio_bridge_event event = {
                .type = SIRIO_BRIDGE_EVENT_REASONING,
                .text = text, .request_id = stream->request_id,
                .model = stream->model,
            };
            if (openai_stream_emit(stream, &event) != 0) {
                free(summary_type);
                free(text);
                return -1;
            }
            stream->reasoning_summary_seen = 1;
        }
        free(summary_type);
        free(text);
        parser_skip_space(&parser);
        if (parser.cursor < parser.end && *parser.cursor == ',') {
            parser.cursor++;
            parser_skip_space(&parser);
        } else if (parser.cursor >= parser.end || *parser.cursor != ']') {
            return openai_stream_fail(stream,
                                      "OpenAI reasoning summary is malformed");
        }
    }
    if (parser.cursor >= parser.end || *parser.cursor != ']')
        return openai_stream_fail(stream, "OpenAI reasoning summary is malformed");
    parser.cursor++;
    parser_skip_space(&parser);
    return parser.cursor == parser.end ? 0 :
        openai_stream_fail(stream, "OpenAI reasoning summary has trailing data");
}

static int openai_stream_usage(openai_stream *stream, json_span response) {
    json_span usage = {0}, detail = {0}, field = {0};
    uint64_t input = 0, output = 0, total = 0, reasoning = 0, cached = 0;
    if (json_object_member(response.start, (size_t)(response.end-response.start),
                           "usage", &usage) != 1)
        return 0;
    if (json_object_member(usage.start, (size_t)(usage.end-usage.start),
                           "input_tokens", &field) == 1)
        json_span_u64(field, &input);
    if (json_object_member(usage.start, (size_t)(usage.end-usage.start),
                           "output_tokens", &field) == 1)
        json_span_u64(field, &output);
    if (json_object_member(usage.start, (size_t)(usage.end-usage.start),
                           "total_tokens", &field) == 1)
        json_span_u64(field, &total);
    if (json_object_member(usage.start, (size_t)(usage.end-usage.start),
                           "output_tokens_details", &detail) == 1 &&
        json_object_member(detail.start, (size_t)(detail.end-detail.start),
                           "reasoning_tokens", &field) == 1)
        json_span_u64(field, &reasoning);
    if (json_object_member(usage.start, (size_t)(usage.end-usage.start),
                           "input_tokens_details", &detail) == 1 &&
        json_object_member(detail.start, (size_t)(detail.end-detail.start),
                           "cached_tokens", &field) == 1)
        json_span_u64(field, &cached);
    sirio_bridge_event event = {
        .type = SIRIO_BRIDGE_EVENT_USAGE,
        .request_id = stream->request_id, .model = stream->model,
        .prompt_tokens = input, .completion_tokens = output,
        .total_tokens = total, .reasoning_tokens = reasoning,
        .prompt_cache_hit_tokens = cached,
        .prompt_cache_miss_tokens = input >= cached ? input - cached : 0,
    };
    return openai_stream_emit(stream, &event);
}

static int openai_stream_complete(openai_stream *stream, json_span response) {
    free(stream->request_id); free(stream->model);
    stream->request_id = json_member_string_dup(response.start,
        (size_t)(response.end-response.start), "id");
    stream->model = json_member_string_dup(response.start,
        (size_t)(response.end-response.start), "model");
    if (openai_stream_usage(stream, response) != 0) return -1;
    if (stream->provider_items_started) {
        if (buffer_putc(&stream->provider_items, ']') != 0)
            return openai_stream_fail(stream, "out of memory");
        sirio_bridge_event state = {
            .type = SIRIO_BRIDGE_EVENT_PROVIDER_STATE,
            .text = stream->provider_items.data,
            .request_id = stream->request_id, .model = stream->model,
        };
        if (openai_stream_emit(stream, &state) != 0) return -1;
    }
    sirio_bridge_event done = {
        .type = SIRIO_BRIDGE_EVENT_DONE,
        .request_id = stream->request_id, .model = stream->model,
        .finish_reason = stream->tool_call_count ? "tool_calls" : "stop",
    };
    if (openai_stream_emit(stream, &done) != 0) return -1;
    stream->completed = 1;
    return 0;
}

static int openai_stream_dispatch_json(openai_stream *stream,
                                       const char *json, size_t length) {
    if (stream->completed)
        return openai_stream_fail(stream,
                                  "data after OpenAI response.completed");
    char *type = json_member_string_dup(json, length, "type");
    if (!type) return openai_stream_fail(stream, "OpenAI SSE event has no type");
    json_span span = {0};
    int result = 0;
    if (!strcmp(type, "response.output_text.delta") ||
        !strcmp(type, "response.reasoning_summary_text.delta")) {
        char *delta = json_member_string_dup(json, length, "delta");
        if (!delta) result = openai_stream_fail(stream, "OpenAI delta is malformed");
        else {
            sirio_bridge_event event = {
                .type = !strcmp(type, "response.output_text.delta") ?
                        SIRIO_BRIDGE_EVENT_TEXT : SIRIO_BRIDGE_EVENT_REASONING,
                .text = delta, .request_id = stream->request_id,
                .model = stream->model,
            };
            result = openai_stream_emit(stream, &event);
            if (result == 0 && event.type == SIRIO_BRIDGE_EVENT_REASONING)
                stream->reasoning_summary_seen = 1;
        }
        free(delta);
    } else if (!strcmp(type, "response.output_item.done")) {
        if (json_object_member(json, length, "item", &span) != 1) {
            result = openai_stream_fail(stream, "OpenAI output item is missing");
        } else if (openai_stream_store_item(stream, span) != 0) {
            result = -1;
        } else {
            char *item_type = json_member_string_dup(span.start,
                (size_t)(span.end-span.start), "type");
            if (item_type && !strcmp(item_type, "reasoning")) {
                result = openai_stream_emit_item_summary(stream, span);
            } else if (item_type && !strcmp(item_type, "function_call")) {
                char *call_id = json_member_string_dup(span.start,
                    (size_t)(span.end-span.start), "call_id");
                char *name = json_member_string_dup(span.start,
                    (size_t)(span.end-span.start), "name");
                char *arguments = json_member_string_dup(span.start,
                    (size_t)(span.end-span.start), "arguments");
                if (!call_id || !name || !arguments) {
                    result = openai_stream_fail(stream,
                                                "incomplete OpenAI function call");
                } else {
                    sirio_bridge_event event = {
                        .type = SIRIO_BRIDGE_EVENT_TOOL_CALL,
                        .tool_call_id = call_id, .tool_name = name,
                        .tool_arguments_json = arguments,
                        .request_id = stream->request_id, .model = stream->model,
                    };
                    result = openai_stream_emit(stream, &event);
                    if (result == 0) stream->tool_call_count++;
                }
                free(call_id); free(name); free(arguments);
            }
            free(item_type);
        }
    } else if (!strcmp(type, "response.completed")) {
        if (json_object_member(json, length, "response", &span) != 1)
            result = openai_stream_fail(stream, "OpenAI completion has no response");
        else result = openai_stream_complete(stream, span);
    } else if (!strcmp(type, "response.failed") ||
               !strcmp(type, "response.incomplete") ||
               !strcmp(type, "error")) {
        char *message = json_member_string_dup(json, length, "message");
        if (!message && json_object_member(json, length, "response", &span) == 1) {
            json_span error = {0};
            if (json_object_member(span.start, (size_t)(span.end-span.start),
                                   "error", &error) == 1)
                message = json_member_string_dup(error.start,
                    (size_t)(error.end-error.start), "message");
        }
        result = openai_stream_fail(stream, "OpenAI response failed: %s",
                                    message ? message : type);
        free(message);
    }
    free(type);
    return result;
}

static int openai_stream_dispatch_event(openai_stream *stream) {
    const char *data = stream->event_data.data ? stream->event_data.data : "";
    size_t length = stream->event_data.length;
    while (length && isspace((unsigned char)*data)) { data++; length--; }
    while (length && isspace((unsigned char)data[length-1])) length--;
    int result = length ? openai_stream_dispatch_json(stream, data, length) : 0;
    stream->event_data.length = 0;
    if (stream->event_data.data) stream->event_data.data[0] = '\0';
    return result;
}

static int openai_stream_line(openai_stream *stream,
                              const char *line, size_t length) {
    if (!length) return openai_stream_dispatch_event(stream);
    if (line[0] == ':') return 0;
    const char *colon = memchr(line, ':', length);
    size_t field_length = colon ? (size_t)(colon-line) : length;
    if (field_length != 4 || memcmp(line, "data", 4)) return 0;
    const char *value = colon ? colon + 1 : line + length;
    size_t value_length = colon ? length - (size_t)(value-line) : 0;
    if (value_length && *value == ' ') { value++; value_length--; }
    if (stream->event_data.length) {
        if (stream->event_data.length >= OPENAI_RESPONSE_LIMIT)
            return openai_stream_fail(stream,
                                      "OpenAI SSE event exceeded limits");
        if (buffer_putc(&stream->event_data, '\n') != 0)
            return openai_stream_fail(stream, "out of memory");
    }
    if (value_length > OPENAI_RESPONSE_LIMIT - stream->event_data.length ||
        buffer_append(&stream->event_data, value, value_length) != 0)
        return openai_stream_fail(stream, "OpenAI SSE event exceeded limits");
    return 0;
}

static int openai_stream_feed(openai_stream *stream,
                              const char *data, size_t length) {
    if (length > OPENAI_RESPONSE_LIMIT - stream->pending.length ||
        buffer_append(&stream->pending, data, length) != 0)
        return openai_stream_fail(stream, "OpenAI SSE buffer exceeded limits");
    size_t consumed = 0;
    while (consumed < stream->pending.length) {
        size_t end = consumed;
        while (end < stream->pending.length && stream->pending.data[end] != '\n' &&
               stream->pending.data[end] != '\r') end++;
        if (end == stream->pending.length) break;
        if (stream->pending.data[end] == '\r' && end + 1 == stream->pending.length)
            break;
        size_t terminator = stream->pending.data[end] == '\r' &&
                            stream->pending.data[end+1] == '\n' ? 2 : 1;
        if (openai_stream_line(stream, stream->pending.data + consumed,
                               end - consumed) != 0) return -1;
        consumed = end + terminator;
    }
    if (consumed) {
        size_t remaining = stream->pending.length - consumed;
        memmove(stream->pending.data, stream->pending.data + consumed, remaining);
        stream->pending.length = remaining;
        stream->pending.data[remaining] = '\0';
    }
    return 0;
}

static size_t openai_stream_header(char *data, size_t size, size_t count,
                                   void *private_data) {
    openai_stream *stream = private_data;
    if (size && count > SIZE_MAX/size) return 0;
    size_t length = size * count;
    static const char prefix[] = "Content-Type:";
    if (length >= sizeof(prefix)-1 &&
        !strncasecmp(data, prefix, sizeof(prefix)-1)) {
        stream->content_type_seen = 1;
        const char *value = data + sizeof(prefix)-1;
        while ((size_t)(value-data) < length && isspace((unsigned char)*value)) value++;
        size_t remaining = length - (size_t)(value - data);
        size_t type_length = strlen("text/event-stream");
        stream->is_sse = remaining >= type_length &&
                         !strncasecmp(value, "text/event-stream", type_length);
    }
    return length;
}

static size_t openai_stream_write(char *data, size_t size, size_t count,
                                  void *private_data) {
    openai_stream *stream = private_data;
    if (size && count > SIZE_MAX/size) return 0;
    size_t length = size * count;
    if (length > OPENAI_RESPONSE_LIMIT - stream->received) {
        openai_stream_fail(stream, "OpenAI response exceeded limits"); return 0;
    }
    stream->received += length;
    if (stream->is_sse) {
        if (openai_stream_feed(stream, data, length) != 0) return 0;
    } else if (buffer_append(&stream->body, data, length) != 0) {
        openai_stream_fail(stream, "out of memory buffering OpenAI response"); return 0;
    }
    return length;
}

/* ChatGPT's Codex transport is an SSE protocol regardless of whether an
 * intermediary preserves its Content-Type header. Wave deliberately parses
 * every successful response as SSE; do the same for data buffered while the
 * header was absent or rewritten. */
static int openai_stream_parse_buffered_body(openai_stream *stream) {
    if (!stream || !stream->body.length) return 0;
    if (openai_stream_feed(stream, stream->body.data,
                           stream->body.length) != 0)
        return -1;
    if (stream->pending.length) {
        if (openai_stream_line(stream, stream->pending.data,
                               stream->pending.length) != 0)
            return -1;
        stream->pending.length = 0;
        stream->pending.data[0] = '\0';
    }
    if (stream->event_data.length)
        return openai_stream_dispatch_event(stream);
    return 0;
}

static void openai_stream_free(openai_stream *stream) {
    buffer_free(&stream->body); buffer_free(&stream->pending);
    buffer_free(&stream->event_data); buffer_free(&stream->provider_items);
    free(stream->request_id); free(stream->model);
    memset(stream, 0, sizeof(*stream));
}

static int openai_generate(sirio_bridge *bridge,
                           const sirio_generation_options *generation,
                           const sirio_message *messages, size_t message_count,
                           const sirio_tool *tools, size_t tool_count,
                           sirio_bridge_event_callback callback,
                           void *private_data) {
    bool oauth = bridge->auth_method == SIRIO_AUTH_OAUTH;
    const char *access_token = bridge->api_key;
    if (oauth) {
        time_t now = time(NULL);
        if (!bridge->openai.access_token ||
            (bridge->openai.access_expires_at &&
             bridge->openai.access_expires_at <= now + OPENAI_REFRESH_MARGIN) ||
            (!bridge->openai.access_expires_at && bridge->openai.refreshed_at &&
             now >= bridge->openai.refreshed_at + 8 * 24 * 60 * 60)) {
            if (openai_auth_refresh(bridge) != 0)
                return emit_bridge_error(bridge, callback, private_data);
        }
        if (!bridge->openai.account_id) {
            bridge_set_error(bridge, "OpenAI ChatGPT account id is unavailable");
            return emit_bridge_error(bridge, callback, private_data);
        }
        access_token = bridge->openai.access_token;
    } else if (bridge->auth_method != SIRIO_AUTH_API_KEY ||
               !access_token || !access_token[0]) {
        bridge_set_error(bridge,
                         "API key authentication is not configured for openai");
        return emit_bridge_error(bridge, callback, private_data);
    }
    bridge_buffer request = {0};
    if (openai_build_request_for_transport(
            &request, bridge->model, !oauth, generation, messages,
            message_count, tools, tool_count) != 0) {
        buffer_free(&request);
        bridge_set_error(bridge, "unable to build OpenAI Responses request");
        return emit_bridge_error(bridge, callback, private_data);
    }
    CURL *curl = curl_easy_init();
    if (!curl) { buffer_free(&request); bridge_set_error(bridge, "unable to create OpenAI request"); return emit_bridge_error(bridge, callback, private_data); }
    size_t auth_size = strlen(access_token) + 23;
    size_t account_size = oauth ? strlen(bridge->openai.account_id) + 21 : 0;
    char *authorization = malloc(auth_size);
    char *account = oauth ? malloc(account_size) : NULL;
    if (!authorization || (oauth && !account)) {
        free(authorization); free(account); curl_easy_cleanup(curl); buffer_free(&request);
        bridge_set_error(bridge, "out of memory"); return emit_bridge_error(bridge, callback, private_data);
    }
    snprintf(authorization, auth_size, "Authorization: Bearer %s", access_token);
    if (oauth)
        snprintf(account, account_size, "ChatGPT-Account-ID: %s",
                 bridge->openai.account_id);
    struct curl_slist *headers = NULL;
    if (bridge_header_append(&headers, "Content-Type: application/json") != 0 ||
        bridge_header_append(&headers, "Accept: text/event-stream") != 0 ||
        (oauth && bridge_header_append(&headers,
            "originator: codex_cli_rs") != 0) ||
        (oauth && bridge_header_append(&headers,
            "x-openai-internal-codex-responses-lite: true") != 0) ||
        bridge_header_append(&headers, authorization) != 0 ||
        (oauth && bridge_header_append(&headers, account) != 0)) {
        curl_slist_free_all(headers);
        free(authorization);
        free(account);
        curl_easy_cleanup(curl);
        buffer_free(&request);
        bridge_set_error(bridge, "unable to allocate HTTP headers");
        return emit_bridge_error(bridge, callback, private_data);
    }
    openai_stream stream = {.bridge=bridge, .callback=callback,
                            .private_data=private_data};
    char curl_error[CURL_ERROR_SIZE] = {0};
    atomic_store_explicit(&bridge->cancelled, false, memory_order_relaxed);
    bridge->last_error[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL,
                     oauth ? OPENAI_CODEX_ENDPOINT : OPENAI_API_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request.length);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, openai_stream_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &stream);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, openai_stream_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, bridge);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sirio");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    CURLcode status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers); free(authorization); free(account);
    curl_easy_cleanup(curl); buffer_free(&request);
    if (status != CURLE_OK) {
        if (stream.callback_rejected) bridge_set_error(bridge, "%s", stream.error);
        else if (atomic_load_explicit(&bridge->cancelled, memory_order_relaxed) ||
                 status == CURLE_ABORTED_BY_CALLBACK)
            bridge_set_error(bridge, "OpenAI request cancelled");
        else if (stream.failed) bridge_set_error(bridge, "%s", stream.error);
        else bridge_set_error(bridge, "OpenAI transport error: %s",
                              curl_error[0] ? curl_error : curl_easy_strerror(status));
        int rejected = stream.callback_rejected;
        openai_stream_free(&stream);
        return rejected ? -1 : emit_bridge_error(bridge, callback, private_data);
    }
    if (http_status < 200 || http_status >= 300) {
        const char *json = stream.body.data ? stream.body.data : "";
        char *message = NULL;
        json_span error = {0};
        if (json_object_member(json, stream.body.length, "error", &error) == 1)
            message = json_member_string_dup(error.start,
                (size_t)(error.end-error.start), "message");
        if (!message)
            message = json_member_string_dup(json, stream.body.length,
                                             "message");
        if (!message)
            message = json_member_string_dup(json, stream.body.length,
                                             "detail");
        bridge_set_error(bridge, "OpenAI HTTP %ld: %s", http_status,
                         message ? message : "request failed");
        free(message); openai_stream_free(&stream);
        return emit_bridge_error(bridge, callback, private_data);
    }
    if (!stream.is_sse && !stream.failed)
        (void)openai_stream_parse_buffered_body(&stream);
    if (stream.pending.length || stream.event_data.length ||
        !stream.completed || stream.failed) {
        char *message = NULL;
        if (stream.body.data) {
            json_span error = {0};
            if (json_object_member(stream.body.data, stream.body.length,
                                   "error", &error) == 1)
                message = json_member_string_dup(
                    error.start, (size_t)(error.end-error.start), "message");
            if (!message)
                message = json_member_string_dup(stream.body.data,
                                                  stream.body.length,
                                                  "message");
            if (!message)
                message = json_member_string_dup(stream.body.data,
                                                  stream.body.length,
                                                  "detail");
        }
        bridge_set_error(bridge, "%s", stream.error[0] ? stream.error :
                         message ? message :
                         !stream.completed ? "OpenAI stream ended without response.completed" :
                         "truncated OpenAI SSE stream");
        free(message);
        int rejected = stream.callback_rejected;
        openai_stream_free(&stream);
        return rejected ? -1 : emit_bridge_error(bridge, callback, private_data);
    }
    openai_stream_free(&stream);
    return 0;
}

static int bridge_header_append(struct curl_slist **headers,
                                const char *value) {
    struct curl_slist *replacement = curl_slist_append(*headers, value);
    if (!replacement) return -1;
    *headers = replacement;
    return 0;
}

struct provider_ops {
    const char *name;
    int (*login)(sirio_bridge *bridge);
    int (*generate)(sirio_bridge *bridge,
                    const sirio_generation_options *generation,
                    const sirio_message *messages, size_t message_count,
                    const sirio_tool *tools, size_t tool_count,
                    sirio_bridge_event_callback callback, void *private_data);
};

static int chat_generate(sirio_bridge *bridge,
                         const sirio_generation_options *generation,
                         const sirio_message *messages,
                         size_t message_count,
                         const sirio_tool *tools, size_t tool_count,
                         sirio_bridge_event_callback callback,
                         void *private_data);

static const struct provider_ops provider_implementations[SIRIO_PROVIDER_COUNT] = {
    [SIRIO_PROVIDER_DEEPSEEK] = {
        .name = "deepseek",
        .login = NULL,
        .generate = chat_generate,
    },
    [SIRIO_PROVIDER_OPENAI] = {
        .name = "openai",
        .login = openai_oauth_login,
        .generate = openai_generate,
    },
    [SIRIO_PROVIDER_OPENCODE_GO] = {
        .name = "opencode-go",
        .login = NULL,
        .generate = chat_generate,
    },
    [SIRIO_PROVIDER_KIMI] = {
        .name = "kimi",
        .login = NULL,
        .generate = chat_generate,
    },
};

int sirio_bridge_load_auth(sirio_bridge *bridge, const char *path) {
    if (!bridge || bridge->provider_id != SIRIO_PROVIDER_OPENAI ||
        bridge->auth_method != SIRIO_AUTH_OAUTH) {
        if (bridge)
            bridge_set_error(bridge,
                             "OAuth credentials require the OpenAI provider");
        return -1;
    }
    const char *target = path && path[0] ? path : bridge->auth_path;
    if (!target || !target[0]) {
        bridge_set_error(bridge, "OpenAI auth path is not configured");
        return -1;
    }
    char error[256] = {0};
    sirio_auth_store *store = sirio_auth_store_load(
        target, error, sizeof(error));
    if (!store) {
        bridge_set_error(bridge, "%s", error[0] ? error :
                         "unable to load authentication");
        return -1;
    }
    openai_auth_free(&bridge->openai);
    if (sirio_auth_store_has(store, SIRIO_PROVIDER_OPENAI,
                             SIRIO_AUTH_OAUTH) &&
        openai_auth_copy(&bridge->openai, &store->openai) != 0) {
        sirio_auth_store_destroy(store);
        bridge_set_error(bridge, "out of memory");
        return -1;
    }
    sirio_auth_store_destroy(store);
    bridge->last_error[0] = '\0';
    return 0;
}

int sirio_bridge_save_auth(sirio_bridge *bridge, const char *path) {
    if (!bridge || bridge->provider_id != SIRIO_PROVIDER_OPENAI ||
        bridge->auth_method != SIRIO_AUTH_OAUTH) {
        if (bridge)
            bridge_set_error(bridge,
                             "OAuth credentials require the OpenAI provider");
        return -1;
    }
    const char *target = path && path[0] ? path : bridge->auth_path;
    if (!target || !target[0]) {
        bridge_set_error(bridge, "OpenAI auth path is not configured");
        return -1;
    }
    if (!bridge->openai.access_token || !bridge->openai.access_token[0]) {
        bridge_set_error(bridge, "OpenAI credentials are empty");
        return -1;
    }
    char error[256] = {0};
    sirio_auth_store *store = sirio_auth_store_load(
        target, error, sizeof(error));
    if (!store) {
        bridge_set_error(bridge, "%s", error[0] ? error :
                         "unable to load authentication");
        return -1;
    }
    if (openai_auth_copy(&store->openai, &bridge->openai) != 0 ||
        sirio_auth_store_set_preferred(store, SIRIO_PROVIDER_OPENAI,
                                       SIRIO_AUTH_OAUTH) != 0 ||
        sirio_auth_store_save(store, target, error, sizeof(error)) != 0) {
        sirio_auth_store_destroy(store);
        bridge_set_error(bridge, "%s", error[0] ? error : "out of memory");
        return -1;
    }
    sirio_auth_store_destroy(store);
    bridge->last_error[0] = '\0';
    return 0;
}

sirio_bridge *sirio_bridge_create(const sirio_bridge_config *config) {
    if (!config) return NULL;
    sirio_bridge *bridge = calloc(1, sizeof(*bridge));
    if (!bridge) return NULL;
    sirio_provider provider = config->provider;
    const sirio_provider_info *provider_info = sirio_provider_get(provider);
    if (!provider_info) {
        free(bridge);
        return NULL;
    }
    sirio_auth_method auth_method = config->auth_method;
    if (auth_method == SIRIO_AUTH_NONE)
        auth_method = provider == SIRIO_PROVIDER_OPENAI ?
                      SIRIO_AUTH_OAUTH : SIRIO_AUTH_API_KEY;
    if ((auth_method == SIRIO_AUTH_API_KEY &&
         !provider_info->supports_api_key) ||
        (auth_method == SIRIO_AUTH_OAUTH && !provider_info->supports_oauth)) {
        free(bridge);
        return NULL;
    }
    const char *model_name = config->model && config->model[0] ?
                             config->model : provider_info->default_model;
    if (!sirio_model_find_for_provider(provider, model_name)) {
        free(bridge);
        return NULL;
    }
    bridge->provider = &provider_implementations[provider];
    bridge->provider_id = provider;
    bridge->auth_method = auth_method;
    atomic_init(&bridge->cancelled, false);
    bridge->api_key = bridge_strdup(config->api_key);
    bridge->model = bridge_strdup(model_name);
    bridge->auth_path = bridge_strdup(config->auth_path);
    if (!bridge->api_key || !bridge->model || !bridge->auth_path) {
        provider_secret_free(bridge->api_key);
        free(bridge->model);
        free(bridge->auth_path);
        free(bridge);
        return NULL;
    }
    bridge->generation.temperature = -1.0;
    bridge->generation.top_p = -1.0;
    bridge->generation.reasoning = SIRIO_REASONING_NONE;
    CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (status == CURLE_OK) {
        bridge->curl_ready = 1;
    } else {
        bridge_set_error(bridge, "libcurl initialization failed: %s",
                         curl_easy_strerror(status));
    }
    if (bridge->curl_ready && provider == SIRIO_PROVIDER_OPENAI &&
        auth_method == SIRIO_AUTH_OAUTH &&
        bridge->auth_path[0] && sirio_bridge_load_auth(bridge, bridge->auth_path) != 0) {
        curl_global_cleanup();
        provider_secret_free(bridge->api_key);
        free(bridge->model);
        free(bridge->auth_path);
        openai_auth_free(&bridge->openai);
        free(bridge);
        return NULL;
    }
    return bridge;
}

int sirio_bridge_authenticate(sirio_bridge *bridge) {
    if (!bridge) return -1;
    bridge->last_error[0] = '\0';
    if (!bridge->curl_ready) {
        bridge_set_error(bridge, "libcurl is not initialized");
        return -1;
    }
    if (bridge->auth_method != SIRIO_AUTH_OAUTH ||
        !bridge->provider->login) {
        bridge_set_error(bridge, "%s does not support OAuth login",
                         sirio_provider_name(bridge->provider_id));
        return -1;
    }
    if (bridge->provider->login(bridge) != 0) return -1;
    if (bridge->provider_id == SIRIO_PROVIDER_OPENAI &&
        bridge->auth_path && bridge->auth_path[0] &&
        sirio_bridge_save_auth(bridge, bridge->auth_path) != 0)
        return -1;
    return 0;
}

static const char *chat_endpoint(sirio_provider provider) {
    switch (provider) {
    case SIRIO_PROVIDER_DEEPSEEK: return DEEPSEEK_ENDPOINT;
    case SIRIO_PROVIDER_OPENCODE_GO: return OPENCODE_GO_ENDPOINT;
    case SIRIO_PROVIDER_KIMI: return KIMI_ENDPOINT;
    default: return NULL;
    }
}

static int chat_generate(sirio_bridge *bridge,
                         const sirio_generation_options *generation,
                         const sirio_message *messages,
                         size_t message_count,
                         const sirio_tool *tools,
                         size_t tool_count,
                         sirio_bridge_event_callback callback,
                         void *private_data) {
    if (!bridge) return -1;
    if (!bridge->curl_ready) return emit_bridge_error(
        bridge, callback, private_data);
    if (bridge->auth_method != SIRIO_AUTH_API_KEY || !bridge->api_key[0]) {
        bridge_set_error(bridge,
                         "API key authentication is not configured for %s",
                         sirio_provider_name(bridge->provider_id));
        return emit_bridge_error(bridge, callback, private_data);
    }

    bridge_buffer request = {0};
    if (build_request_for_model(&request, bridge->provider_id,
                                bridge->model, generation,
                                messages, message_count, tools,
                                tool_count) != 0) {
        buffer_free(&request);
        bridge_set_error(bridge, "unable to build %s request",
                         sirio_provider_name(bridge->provider_id));
        return emit_bridge_error(bridge, callback, private_data);
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        buffer_free(&request);
        bridge_set_error(bridge, "unable to create libcurl request");
        return emit_bridge_error(bridge, callback, private_data);
    }

    size_t auth_length = strlen(bridge->api_key) + 23;
    char *authorization = malloc(auth_length);
    if (!authorization) {
        curl_easy_cleanup(curl);
        buffer_free(&request);
        bridge_set_error(bridge, "out of memory");
        return emit_bridge_error(bridge, callback, private_data);
    }
    snprintf(authorization, auth_length, "Authorization: Bearer %s",
             bridge->api_key);

    struct curl_slist *headers = NULL;
    if (bridge_header_append(&headers, "Content-Type: application/json") != 0 ||
        bridge_header_append(&headers, "Accept: text/event-stream") != 0 ||
        bridge_header_append(&headers, authorization) != 0) {
        curl_slist_free_all(headers);
        free(authorization);
        curl_easy_cleanup(curl);
        buffer_free(&request);
        bridge_set_error(bridge, "unable to allocate HTTP headers");
        return emit_bridge_error(bridge, callback, private_data);
    }

    stream_response response = {
        .bridge = bridge,
        .callback = callback,
        .private_data = private_data,
    };
    char curl_error[CURL_ERROR_SIZE] = {0};
    atomic_store_explicit(&bridge->cancelled, false, memory_order_relaxed);
    bridge->last_error[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, chat_endpoint(bridge->provider_id));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)request.length);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, stream_http_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, bridge);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sirio");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

    CURLcode curl_status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    free(authorization);
    curl_easy_cleanup(curl);
    buffer_free(&request);

    if (curl_status != CURLE_OK) {
        if (response.callback_rejected) {
            bridge_set_error(bridge, "%s", response.error[0] ?
                             response.error :
                             "agent rejected a Chat Completions event");
            stream_response_free(&response);
            return -1;
        } else if (atomic_load_explicit(&bridge->cancelled,
                                        memory_order_relaxed) ||
            curl_status == CURLE_ABORTED_BY_CALLBACK) {
            bridge_set_error(bridge, "%s request cancelled",
                             sirio_provider_name(bridge->provider_id));
        } else if (response.failed) {
            bridge_set_error(bridge, "%s", response.error[0] ?
                             response.error : "Chat Completions stream failed");
        } else {
            bridge_set_error(bridge, "%s transport error: %s",
                             sirio_provider_name(bridge->provider_id),
                             curl_error[0] ? curl_error :
                             curl_easy_strerror(curl_status));
        }
        stream_response_free(&response);
        return emit_bridge_error(bridge, callback, private_data);
    }

    if (http_status < 200 || http_status >= 300) {
        deepseek_result result = {0};
        char parse_error[160] = {0};
        int parsed = parse_response(response.body.data ? response.body.data : "",
                                    response.body.length, &result,
                                    parse_error, sizeof(parse_error));
        bridge_set_error(bridge, "%s HTTP %ld: %s",
                         sirio_provider_name(bridge->provider_id), http_status,
                         parsed == 0 && result.error_message ?
                         result.error_message :
                         parse_error[0] ? parse_error : "request failed");
        result_free(&result);
        stream_response_free(&response);
        return emit_bridge_error(bridge, callback, private_data);
    }

    if (!response.content_type_seen || !response.is_sse) {
        bridge_set_error(bridge, "%s",
                         response.content_type_seen ?
                         "Chat Completions success response is not text/event-stream" :
                         "Chat Completions success response has no Content-Type");
        stream_response_free(&response);
        return emit_bridge_error(bridge, callback, private_data);
    }

    if (stream_finish(&response) != 0) {
        bridge_set_error(bridge, "%s", response.error[0] ?
                         response.error : "Chat Completions stream failed");
        int callback_rejected = response.callback_rejected;
        stream_response_free(&response);
        if (callback_rejected) return -1;
        return emit_bridge_error(bridge, callback, private_data);
    }
    stream_response_free(&response);
    return 0;
}

int sirio_bridge_generate(sirio_bridge *bridge,
                          const sirio_message *messages,
                          size_t message_count,
                          const sirio_tool *tools,
                          size_t tool_count,
                          sirio_bridge_event_callback callback,
                          void *private_data) {
    if (!bridge) return -1;
    return bridge->provider->generate(
        bridge, &bridge->generation, messages, message_count,
        tools, tool_count, callback, private_data);
}

int sirio_bridge_generate_with_options(
        sirio_bridge *bridge, const sirio_generation_options *options,
        const sirio_message *messages, size_t message_count,
        const sirio_tool *tools, size_t tool_count,
        sirio_bridge_event_callback callback, void *private_data) {
    if (!bridge) return -1;
    sirio_generation_options prepared;
    if (bridge_prepare_generation_options(bridge, options, &prepared) != 0)
        return emit_bridge_error(bridge, callback, private_data);
    return bridge->provider->generate(
        bridge, &prepared, messages, message_count,
        tools, tool_count, callback, private_data);
}

void sirio_bridge_cancel(sirio_bridge *bridge) {
    if (bridge)
        atomic_store_explicit(&bridge->cancelled, true, memory_order_relaxed);
}

int sirio_bridge_set_generation_options(
        sirio_bridge *bridge, const sirio_generation_options *options) {
    sirio_generation_options prepared;
    if (bridge_prepare_generation_options(bridge, options, &prepared) != 0)
        return -1;
    bridge->generation = prepared;
    bridge->last_error[0] = '\0';
    return 0;
}

void sirio_bridge_set_cancel_poll(sirio_bridge *bridge,
                                  sirio_bridge_cancel_poll poll, void *priv) {
    if (!bridge) return;
    bridge->cancel_poll = poll;
    bridge->cancel_poll_priv = priv;
}

void sirio_bridge_destroy(sirio_bridge *bridge) {
    if (!bridge) return;
    provider_secret_free(bridge->api_key);
    free(bridge->model);
    free(bridge->auth_path);
    openai_auth_free(&bridge->openai);
    if (bridge->curl_ready) curl_global_cleanup();
    free(bridge);
}

const char *sirio_bridge_last_error(const sirio_bridge *bridge) {
    return bridge ? bridge->last_error : "bridge is not initialized";
}

const char *sirio_role_name(sirio_role role) {
    switch (role) {
    case SIRIO_ROLE_SYSTEM: return "system";
    case SIRIO_ROLE_USER: return "user";
    case SIRIO_ROLE_ASSISTANT: return "assistant";
    case SIRIO_ROLE_TOOL: return "tool";
    }
    return "unknown";
}
