/*
 * sirio_worker.h - public API for the autonomous Sirio worker module.
 *
 * The engine-bound functions separated from the upstream DS4 core are re-added,
 * patched to run against the Sirio bridge (API in sirio_provider.h) instead of the
 * upstream local engine.  sirio_worker.c is compiled independently into
 * sirio_worker.o.  Its public integration seam connects it to the retained
 * agent core without including either C source from the other.
 *
 * The sirio_engine type stays opaque in sirio_core.h, but this header
 * completes it: in Sirio an "engine" is just a wrapper around the bridge.
 * agent_worker_init() receives the engine pointer from the kept runtime
 * loop (run_agent) and stores the bridge for the worker thread.
 */

#ifndef SIRIO_WORKER_H
#define SIRIO_WORKER_H

#include "sirio_core.h"
#include "sirio_container.h"
#include "sirio_provider.h"
#include "linenoise.h"

_Static_assert((int)SIRIO_THINK_NONE == (int)SIRIO_REASONING_NONE &&
               (int)SIRIO_THINK_MAX == (int)SIRIO_REASONING_MAX,
               "agent and provider reasoning enums must stay aligned");

typedef int (*sirio_engine_select_fn)(
    sirio_engine *engine, const char *model, const char *reasoning,
    char *error, size_t error_len);
typedef int (*sirio_engine_step_model_fn)(
    sirio_engine *engine, int direction, bool *at_limit,
    char *error, size_t error_len);

/* Selection callbacks return 0 on success, 1 for an operational failure
 * (credentials, I/O, provider setup), and 2 for an invalid selection. */

/* The host owns credentials and models.json. The worker owns interaction and
 * invokes select only at idle safe points, allowing sirio.c to build a new
 * bridge and swap it transactionally. */
struct sirio_engine {
    sirio_bridge *bridge;
    sirio_provider provider;
    const sirio_model_info *model;
    const sirio_model_store *models;
    sirio_reasoning_effort reasoning;
    sirio_generation_options generation;
    sirio_bridge_cancel_poll cancel_poll;
    void *cancel_poll_private_data;
    sirio_engine_select_fn select;
    sirio_engine_step_model_fn step_model;
    void *select_private_data;
};

typedef struct {
    char sha[41];
    char provider[32];
    char model[128];
    char reasoning[16];
    char title[257];
} sirio_session_info;

/* Worker side of the separately linked core/worker integration contract. */
bool agent_worker_save_session_now(agent_worker *worker, char sha_out[41],
                                   int *tokens_out,
                                   char *error, size_t error_len);
bool agent_worker_find_session(agent_worker *worker, const char *prefix,
                               char sha_out[41], char **path_out,
                               char *error, size_t error_len);
int agent_worker_effective_ctx_size(const agent_worker *worker);
bool agent_worker_compact(agent_worker *worker, const char *reason,
                          char *error, size_t error_len);
int agent_worker_init(agent_worker *worker, sirio_engine *engine,
                      agent_config *config);
void agent_worker_free(agent_worker *worker);
void sirio_worker_set_completion_context(agent_worker *worker);
void agent_switch_completion_callback(const char *buf,
                                      linenoiseCompletions *completions);
void agent_worker_list_sessions(agent_worker *worker);
bool agent_worker_reset_to_sysprompt(agent_worker *worker,
                                     char *error, size_t error_len);
bool agent_worker_switch_session(agent_worker *worker, const char *prefix,
                                 int history_turns,
                                 char *error, size_t error_len);
bool agent_worker_strip_session(agent_worker *worker, const char *prefix,
                                char sha_out[41], uint32_t *tokens_out,
                                char *error, size_t error_len);
bool agent_worker_show_history(agent_worker *worker, int user_turns,
                               char *error, size_t error_len);
bool agent_worker_select_model(agent_worker *worker, const char *model,
                               const char *reasoning,
                               char *error, size_t error_len);
bool agent_worker_step_reasoning(agent_worker *worker, int direction,
                                 bool *at_limit,
                                 char *error, size_t error_len);
bool agent_worker_step_model(agent_worker *worker, int direction,
                             bool *at_limit,
                             char *error, size_t error_len);
int sirio_sessions_list(FILE *output, char *error, size_t error_len);
int sirio_session_inspect(const char *prefix, sirio_session_info *info,
                          char *error, size_t error_len);
int sirio_session_delete(const char *prefix, sirio_session_info *info,
                         char *error, size_t error_len);
int sirio_session_strip(const char *prefix, sirio_session_info *info,
                        uint32_t *tokens_out,
                        char *error, size_t error_len);
void runtime_help(void);
void build_status_text(const agent_status *status, char *buf, size_t len);
void test_agent_cache_rejects_impossible_lengths(void);

/*
 * Entry point used by sirio.c: builds the agent configuration, wraps the
 * bridge into a sirio_engine, and starts the interactive or non-interactive
 * runtime loop retained in sirio_core.c.
 */
int sirio_agent_run(sirio_engine *engine, const agent_config *config);

/* Side-effect-free syntax validation used by the sole application entry
 * point before reading credentials or selecting a provider. */
int sirio_agent_parse_options(agent_config *config, int argc, char **argv,
                              char *error, size_t error_len);

#endif /* SIRIO_WORKER_H */
