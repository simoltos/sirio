/* Shared agent/worker declarations. Sirio began from the DS4 Agent layer but
 * this interface describes only the current provider and container runtime. */

#ifndef SIRIO_CORE_H
#define SIRIO_CORE_H

#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sirio_container.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SIRIO_SUBAGENT_DEPTH_ENV "SIRIO_SUBAGENT_DEPTH"

/* Opaque inference engine handle. */
typedef struct sirio_engine sirio_engine;

typedef enum {
    SIRIO_THINK_NONE,
    SIRIO_THINK_LOW,
    SIRIO_THINK_MEDIUM,
    SIRIO_THINK_HIGH,
    SIRIO_THINK_XHIGH,
    SIRIO_THINK_MAX,
} sirio_think_mode;

/* Shared core/worker state. The worker owns its lifecycle and the agent owns
 * terminal I/O and rendering. */
typedef struct {
    const char *prompt;
    const char *system;
    const char *trace_path;
    bool raw_prompt;
    int n_predict;
    int ctx_size;
    float temperature;
    float top_p;
    bool temperature_set;
    bool top_p_set;
    bool tokens_set;
    bool reasoning_set;
    bool system_set;
    sirio_think_mode think_mode;
} agent_generation_options;

typedef struct {
    agent_generation_options gen;
    const char *executable_path;
    const char *chdir_path;
    bool non_interactive;
    bool edit_upto;
    const char *resume_session;
    int resume_history_turns;
    sirio_container *external_tools;
} agent_config;

typedef enum {
    AGENT_WORKER_IDLE,
    AGENT_WORKER_PREFILL,
    AGENT_WORKER_GENERATING,
    AGENT_WORKER_COMPACTING,
    AGENT_WORKER_DRAINING,
    AGENT_WORKER_SAVING,
    AGENT_WORKER_ERROR,
    AGENT_WORKER_STOPPED,
} agent_worker_state;

typedef struct {
    agent_worker_state state;
    int prefill_done;
    int prefill_total;
    unsigned prefill_label;
    double prefill_tps;
    int generated;
    double gen_tps;
    bool greedy_sampling;
    int ctx_used;
    int ctx_size;
    char provider[16];
    char model[64];
    char reasoning[16];
    char error[256];
} agent_status;

typedef struct agent_bash_job agent_bash_job;

typedef struct {
    sirio_engine *engine;
    agent_config *cfg;
    int context_used;
    char *cache_dir;
    char session_sha[41];
    char *session_title;
    uint64_t session_created_at;
    bool user_activity;
    bool session_dirty;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cond;
    int wake_fd[2];
    FILE *trace;
    bool wake_pending;
    bool stop;
    bool interrupt;
    bool initialized;
    bool save_requested;
    bool compact_requested;
    int progress_base;
    bool progress_direct;
    double progress_started_at;
    char *cmd_text;
    agent_status status;
    char *out;
    size_t out_len;
    size_t out_cap;
    bool queued_user_drain_pending;
    bool queued_user_drain_answered;
    char *queued_user_drain_text;
    bool datetime_context_injected;
    int last_system_prompt_reminder_at;
    bool raw_mode_needs_restore;
} agent_worker;

typedef enum {
    AGENT_MD_PENDING_NONE,
    AGENT_MD_PENDING_STAR,
    AGENT_MD_PENDING_BACKTICK,
} agent_markdown_pending;

typedef struct agent_syntax agent_syntax;

typedef struct {
    sirio_engine *engine;
    agent_worker *worker;
    bool format_thinking;
    bool format_markdown;
    bool in_think;
    bool color_open;
    bool use_color;
    bool last_output_newline;
    bool wrote_visible_output;
    bool md_bold;
    bool md_italic;
    bool md_inline_code;
    bool md_code_block;
    bool md_fence_info;
    bool md_code_line_start;
    bool md_code_in_ml_comment;
    bool md_syntax_silent;
    bool md_syntax_has_highlight;
    agent_markdown_pending md_pending;
    size_t md_pending_len;
    const agent_syntax *md_syntax;
    char md_fence_lang[32];
    size_t md_fence_lang_len;
    const char *md_code_line_prefix;
    const char *md_code_line_prefix_color;
    bool md_code_highlight_upto;
    char *md_code_line;
    size_t md_code_line_len;
    size_t md_code_line_cap;
    char pending[16];
    size_t pending_len;
    char utf8_pending[4];
    size_t utf8_pending_len;
    size_t utf8_pending_need;
} agent_token_renderer;

typedef struct {
    char *name;
    char *value;
    bool is_string;
} agent_tool_arg;

typedef struct {
    char *name;
    agent_tool_arg *args;
    int argc;
    int argcap;
} agent_tool_call;

typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
    bool truncated;
} agent_buf;

typedef struct {
    char sha[41];
    uint64_t last_used;
} agent_completion_session;

typedef struct {
    agent_completion_session *v;
    int len;
    int cap;
} agent_completion_sessions;

#define AGENT_SYSTEM_PROMPT_REMINDER_TOKENS 50000
#define AGENT_COMPACT_TAIL_DIVISOR 10
#define AGENT_COMPACT_TAIL_CAP_TOKENS 50000
#define AGENT_COMPACT_SUMMARY_MAX_TOKENS 4096
#define AGENT_COMPACT_SOFT_PERCENT 85
#define AGENT_COMPACT_MIN_FREE_TOKENS 8192

/* Services supplied by the retained agent core to the worker object. */
int set_nonblock(int fd, bool on, int *old_flags);
void *xmalloc(size_t n);
void *xrealloc(void *ptr, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void usage(FILE *fp, const char *topic);
void agent_sigint_handler(int sig);
void agent_worker_note_system_prompt_seen(agent_worker *w);
void agent_wake_locked(agent_worker *w);
void agent_publish(agent_worker *w, const char *s, size_t n);
void agent_publishf(agent_worker *w, const char *fmt, ...);
void agent_publish_tool_observation(agent_worker *w, const char *observation);
void agent_set_status(agent_worker *w, agent_worker_state state);
void agent_set_error(agent_worker *w, const char *msg);
void agent_trace(agent_worker *w, const char *fmt, ...);
void agent_trace_text(agent_worker *w, const char *label,
                      const char *text, size_t len);
void renderer_process(agent_token_renderer *renderer, const char *text,
                      size_t len, bool finish);
void renderer_finish(agent_token_renderer *renderer);
bool worker_should_interrupt(agent_worker *w);
void worker_clear_interrupt(agent_worker *w);
bool agent_err_is_interrupted(const char *err);
void agent_buf_puts(agent_buf *buf, const char *text);
char *agent_buf_take(agent_buf *buf);
bool agent_mkdir_p(const char *path);
void agent_le_put64(uint8_t *p, uint64_t value);
void agent_worker_clear_session_identity(agent_worker *w);
void agent_publish_system_status(agent_worker *w, const char *msg);
void agent_publishf_system_status(agent_worker *w, const char *fmt, ...);
char *worker_request_queued_user_drain(agent_worker *w);
void agent_format_age(uint64_t when, char *buf, size_t len);
char *agent_session_title_from_prompt(const char *prompt, size_t max_bytes);
char *agent_session_title_clip(const char *title, size_t max_bytes);
void agent_completion_sessions_push(agent_completion_sessions *sessions,
                                    const char sha[41], uint64_t last_used);
int agent_completion_session_cmp(const void *a, const void *b);
char *agent_compact_make_prompt(const char *reason);
bool agent_worker_compact_if_needed(agent_worker *w, const char *reason,
                                    char *err, size_t err_len);
void worker_run_deferred_save(agent_worker *w);
void worker_run_deferred_compact(agent_worker *w);
void worker_stop(agent_worker *w);
bool worker_is_idle(agent_worker *w);
void agent_format_ctx_size(int ctx_size, char *buf, size_t len);
int run_agent_non_interactive(sirio_engine *engine, agent_config *cfg);
int run_agent(sirio_engine *engine, agent_config *cfg);

int sirio_help_print(FILE *fp, const char *topic);

#endif /* SIRIO_CORE_H */
