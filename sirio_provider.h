#ifndef SIRIO_PROVIDER_H
#define SIRIO_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Conservative decimal limits for the initial DeepSeek model.  They remain
 * available for code derived from the original adapter, but are not Sirio-wide
 * limits: provider/model metadata below is authoritative at runtime. */
#define SIRIO_MODEL_CONTEXT_TOKENS 1000000
#define SIRIO_MODEL_MAX_OUTPUT_TOKENS 384000

typedef enum {
    SIRIO_PROVIDER_NONE = 0,
    SIRIO_PROVIDER_DEEPSEEK,
    SIRIO_PROVIDER_OPENAI,
    SIRIO_PROVIDER_OPENCODE_GO,
    SIRIO_PROVIDER_KIMI,
    SIRIO_PROVIDER_COUNT
} sirio_provider;

typedef enum {
    SIRIO_ROLE_SYSTEM,
    SIRIO_ROLE_USER,
    SIRIO_ROLE_ASSISTANT,
    SIRIO_ROLE_TOOL
} sirio_role;

/* Provider-neutral tool/message IR. Provider adapters translate these fields
 * to their wire vocabulary (for example, Chat Completions'
 * `reasoning_content` and OpenAI's function wrapper). JSON remains encoded at
 * this boundary so another model adapter never depends on Sirio's local
 * executor structs. */
typedef struct {
    char *id;
    char *name;
    char *arguments_json;
} sirio_tool_call;

typedef struct {
    sirio_role role;
    sirio_provider provider;
    char *content;
    char *reasoning;
    /* Optional provider-owned JSON needed for an exact continuation. The
     * worker stores and replays it verbatim; adapters other than its owner
     * must ignore it. */
    char *provider_state_json;
    char *tool_call_id;
    sirio_tool_call *tool_calls;
    size_t tool_call_count;
} sirio_message;

typedef enum {
    SIRIO_TOOL_ARGUMENT_STRING,
    SIRIO_TOOL_ARGUMENT_NUMBER,
    SIRIO_TOOL_ARGUMENT_BOOLEAN
} sirio_tool_argument_type;

typedef struct {
    char *name;
    char *value;
    sirio_tool_argument_type type;
} sirio_tool_argument;

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
} sirio_tool;

typedef struct sirio_bridge sirio_bridge;

typedef enum {
    SIRIO_AUTH_NONE,
    SIRIO_AUTH_API_KEY,
    SIRIO_AUTH_OAUTH
} sirio_auth_method;

typedef enum {
    SIRIO_REASONING_NONE = 0,
    SIRIO_REASONING_LOW,
    SIRIO_REASONING_MEDIUM,
    SIRIO_REASONING_HIGH,
    SIRIO_REASONING_XHIGH,
    SIRIO_REASONING_MAX,
    SIRIO_REASONING_COUNT
} sirio_reasoning_effort;

#define SIRIO_REASONING_BIT(effort) (1u << (unsigned)(effort))

typedef struct {
    sirio_provider id;
    const char *name;
    const char *default_model;
    bool supports_api_key;
    bool supports_oauth;
} sirio_provider_info;

typedef struct {
    const char *name;
    sirio_provider provider;
    int context_tokens;
    int max_output_tokens;
    unsigned reasoning_mask;
    sirio_reasoning_effort default_reasoning;
} sirio_model_info;

size_t sirio_provider_count(void);
const sirio_provider_info *sirio_provider_at(size_t index);
const sirio_provider_info *sirio_provider_get(sirio_provider provider);
const sirio_provider_info *sirio_provider_find(const char *name);
const char *sirio_provider_name(sirio_provider provider);

size_t sirio_model_count(void);
const sirio_model_info *sirio_model_at(size_t index);
const sirio_model_info *sirio_model_find(const char *name);
const sirio_model_info *sirio_model_find_for_provider(
    sirio_provider provider, const char *name);
const sirio_model_info *sirio_model_resolve(
    sirio_provider provider_hint, const char *name,
    char *error, size_t error_len);
const char *sirio_reasoning_name(sirio_reasoning_effort effort);
bool sirio_reasoning_parse(const char *name,
                           sirio_reasoning_effort *effort_out);
bool sirio_model_supports_reasoning(const sirio_model_info *model,
                                    sirio_reasoning_effort effort);
bool sirio_model_step_reasoning(const sirio_model_info *model,
                                sirio_reasoning_effort current,
                                int direction,
                                sirio_reasoning_effort *next_out);

/* default.json stores only the ordered interface model set and its last
 * selection. Model metadata remains authoritative in the compiled catalog.
 * Loading a missing file succeeds with an empty store. */
typedef struct sirio_default_store sirio_default_store;

sirio_default_store *sirio_default_store_load(
    const char *path, char *error, size_t error_len);
int sirio_default_store_save(const sirio_default_store *store,
                             const char *path,
                             char *error, size_t error_len);
void sirio_default_store_destroy(sirio_default_store *store);
int sirio_default_store_validate(const sirio_default_store *store,
                                 char *error, size_t error_len);
size_t sirio_default_store_count(const sirio_default_store *store);
const sirio_model_info *sirio_default_store_at(
    const sirio_default_store *store, size_t index);
bool sirio_default_store_contains(const sirio_default_store *store,
                                  const sirio_model_info *model);
const sirio_model_info *sirio_default_store_resolve(
    const sirio_default_store *store, const char *name,
    char *error, size_t error_len);
sirio_reasoning_effort sirio_default_store_reasoning(
    const sirio_default_store *store, const sirio_model_info *model);
bool sirio_default_store_last_used(
    const sirio_default_store *store,
    const sirio_model_info **model_out,
    sirio_reasoning_effort *reasoning_out);
int sirio_default_store_set_last_used(
    sirio_default_store *store, const sirio_model_info *model,
    sirio_reasoning_effort reasoning);
int sirio_default_store_add(sirio_default_store *store,
                            const sirio_model_info *model,
                            bool *changed_out);
int sirio_default_store_remove(sirio_default_store *store,
                               const char *reference,
                               bool *changed_out);

/* Opaque host-side configuration and credential store.  Loading a missing
 * file succeeds with an empty store.  Saving creates parent directories and
 * atomically replaces an owner-only auth.json. */
typedef struct sirio_auth_store sirio_auth_store;

sirio_auth_store *sirio_auth_store_load(const char *path,
                                        char *error, size_t error_len);
int sirio_auth_store_save(const sirio_auth_store *store, const char *path,
                          char *error, size_t error_len);
void sirio_auth_store_destroy(sirio_auth_store *store);

bool sirio_auth_store_has(const sirio_auth_store *store,
                          sirio_provider provider,
                          sirio_auth_method method);
sirio_auth_method sirio_auth_store_preferred(
    const sirio_auth_store *store, sirio_provider provider);
const char *sirio_auth_store_api_key(const sirio_auth_store *store,
                                     sirio_provider provider);
time_t sirio_auth_store_oauth_expiry(const sirio_auth_store *store,
                                     sirio_provider provider);
int sirio_auth_store_set_api_key(sirio_auth_store *store,
                                 sirio_provider provider,
                                 const char *api_key);
int sirio_auth_store_set_preferred(sirio_auth_store *store,
                                   sirio_provider provider,
                                   sirio_auth_method method);
void sirio_auth_store_clear_provider(sirio_auth_store *store,
                                     sirio_provider provider);

typedef struct {
    const char *api_key;
    sirio_provider provider;
    sirio_auth_method auth_method;
    const char *model;
    /* Host-side unified credential file. The bridge does not pass this path
     * or its contents to the runner or save them in a conversation. */
    const char *auth_path;
} sirio_bridge_config;

typedef enum {
    SIRIO_BRIDGE_EVENT_TEXT,
    SIRIO_BRIDGE_EVENT_REASONING,
    SIRIO_BRIDGE_EVENT_TOOL_CALL,
    SIRIO_BRIDGE_EVENT_PROVIDER_STATE,
    SIRIO_BRIDGE_EVENT_USAGE,
    SIRIO_BRIDGE_EVENT_DONE,
    SIRIO_BRIDGE_EVENT_ERROR
} sirio_bridge_event_type;

typedef struct {
    sirio_bridge_event_type type;
    const char *text;
    const char *tool_call_id;
    const char *tool_name;
    const char *tool_arguments_json;
    const char *error;
    const char *request_id;
    const char *model;
    const char *finish_reason;
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    uint64_t total_tokens;
    uint64_t reasoning_tokens;
    uint64_t prompt_cache_hit_tokens;
    uint64_t prompt_cache_miss_tokens;
} sirio_bridge_event;

typedef int (*sirio_bridge_event_callback)(const sirio_bridge_event *event,
                                           void *private_data);

sirio_bridge *sirio_bridge_create(const sirio_bridge_config *config);

/* Load or merge this bridge's host-side OpenAI OAuth credentials through the
 * unified store.  A missing file is a successful empty load; malformed or
 * unreadable files fail. Other providers, API keys, and defaults are preserved
 * on save. */
int sirio_bridge_load_auth(sirio_bridge *bridge, const char *path);
int sirio_bridge_save_auth(sirio_bridge *bridge, const char *path);

/* Perform the selected provider's interactive authentication, when needed. */
int sirio_bridge_authenticate(sirio_bridge *bridge);

int sirio_bridge_generate(sirio_bridge *bridge,
                          const sirio_message *messages,
                          size_t message_count,
                          const sirio_tool *tools,
                          size_t tool_count,
                          sirio_bridge_event_callback callback,
                          void *private_data);

void sirio_bridge_cancel(sirio_bridge *bridge);

/* Optional out-of-band cancel hook: the bridge asks poll(priv) from its
 * transfer progress callback, so a caller can abort an in-flight request
 * without owning a bridge reference (e.g. the agent's Ctrl+C latch). */
typedef int (*sirio_bridge_cancel_poll)(void *priv);
void sirio_bridge_set_cancel_poll(sirio_bridge *bridge,
                                  sirio_bridge_cancel_poll poll, void *priv);

typedef struct {
    int max_tokens;            /* <= 0: omit and use the provider default */
    double temperature;        /* < 0: omit and use the provider default */
    double top_p;              /* < 0: omit and use the provider default */
    sirio_reasoning_effort reasoning;
    bool max_tokens_explicit;
    bool temperature_explicit;
    bool top_p_explicit;
} sirio_generation_options;

/* Validate and copy options used by subsequent generation requests. */
int sirio_bridge_set_generation_options(
    sirio_bridge *bridge, const sirio_generation_options *options);

int sirio_bridge_generate_with_options(
    sirio_bridge *bridge,
    const sirio_generation_options *options,
    const sirio_message *messages,
    size_t message_count,
    const sirio_tool *tools,
    size_t tool_count,
    sirio_bridge_event_callback callback,
    void *private_data);

void sirio_bridge_destroy(sirio_bridge *bridge);
const char *sirio_bridge_last_error(const sirio_bridge *bridge);

/* Decode one provider function-call arguments object. The accepted contract
 * deliberately matches Sirio's flat tool schemas: string, number, and boolean
 * property values only. Duplicate names and nested/null values are rejected. */
int sirio_tool_arguments_parse(const char *json,
                               sirio_tool_argument **arguments_out,
                               size_t *argument_count_out,
                               char *error, size_t error_len);
/* Decode arguments and enforce the portable flat-object JSON Schema subset
 * used by Sirio tools: string/number/integer/boolean properties, required,
 * and additionalProperties. */
int sirio_tool_arguments_parse_validated(
    const char *arguments_json,
    const char *input_schema_json,
    sirio_tool_argument **arguments_out,
    size_t *argument_count_out,
    char *error, size_t error_len);
void sirio_tool_arguments_free(sirio_tool_argument *arguments,
                               size_t argument_count);

void sirio_message_free(sirio_message *message);
const char *sirio_role_name(sirio_role role);

#endif /* SIRIO_PROVIDER_H */
