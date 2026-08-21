/* Sirio application entry point.  Provider implementations and the unified
 * credential store live in sirio_provider.c; the agent runtime lives in the
 * autonomous worker module. */

#include "sirio_provider.h"
#include "sirio_worker.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

typedef enum {
    SIRIO_CLI_RUN,
    SIRIO_CLI_API_KEY,
    SIRIO_CLI_LOGIN,
    SIRIO_CLI_LOGOUT,
    SIRIO_CLI_AUTH_STATUS,
    SIRIO_CLI_LIST_PROVIDERS,
    SIRIO_CLI_LIST_MODELS,
    SIRIO_CLI_ADD_DEFAULT,
    SIRIO_CLI_REMOVE_DEFAULT,
    SIRIO_CLI_SESSIONS,
    SIRIO_CLI_DELETE_SESSION,
    SIRIO_CLI_STRIP_SESSION,
} sirio_cli_action;

typedef enum {
    SIRIO_CLI_COMMAND_NONE,
    SIRIO_CLI_COMMAND_AUTH,
    SIRIO_CLI_COMMAND_CATALOG,
    SIRIO_CLI_COMMAND_SESSIONS,
} sirio_cli_command;

typedef struct {
    sirio_cli_command command;
    sirio_cli_action action;
    bool command_action_set;
    const char *action_value;
    const char *provider_name;
    const char *model_name;
    const char *resume_session;
    int resume_turns;
    bool resume_turns_set;
    bool secret_from_stdin;
    bool assume_yes;
    char **agent_argv;
    int agent_argc;
} sirio_cli_options;

static const char *sirio_cli_command_name(sirio_cli_command command) {
    switch (command) {
    case SIRIO_CLI_COMMAND_AUTH: return "auth";
    case SIRIO_CLI_COMMAND_CATALOG: return "catalog";
    case SIRIO_CLI_COMMAND_SESSIONS: return "sessions";
    case SIRIO_CLI_COMMAND_NONE: return NULL;
    }
    return NULL;
}

static sirio_cli_command sirio_cli_command_parse(const char *name) {
    if (!strcmp(name, "auth")) return SIRIO_CLI_COMMAND_AUTH;
    if (!strcmp(name, "catalog")) return SIRIO_CLI_COMMAND_CATALOG;
    if (!strcmp(name, "sessions")) return SIRIO_CLI_COMMAND_SESSIONS;
    return SIRIO_CLI_COMMAND_NONE;
}

static void sirio_cli_error(const char *message) {
    fprintf(stderr, "sirio: %s\n", message);
}

static bool sirio_executable_path(const char *argv0,
                                  char path[PATH_MAX]) {
    if (!argv0 || !argv0[0]) return false;
    if (strchr(argv0, '/') && realpath(argv0, path)) return true;
    int length = snprintf(path, PATH_MAX, "%s", argv0);
    return length >= 0 && length < PATH_MAX;
}

static bool sirio_agent_option_has_value(const char *option) {
    return !strcmp(option, "-p") || !strcmp(option, "--prompt") ||
           !strcmp(option, "-sys") || !strcmp(option, "--system") ||
           !strcmp(option, "-n") || !strcmp(option, "--tokens") ||
           !strcmp(option, "--trace") || !strcmp(option, "-C") ||
           !strcmp(option, "--chdir") || !strcmp(option, "--temp") ||
           !strcmp(option, "--top-p") || !strcmp(option, "--think");
}

static bool sirio_agent_option_is_flag(const char *option) {
    return !strcmp(option, "--non-interactive") ||
           !strcmp(option, "--raw-prompt") ||
           !strcmp(option, "--edit-upto");
}

static const char *sirio_cli_value(int *index, int argc, char **argv,
                                   char *error, size_t error_len) {
    if (*index + 1 < argc) return argv[++(*index)];
    snprintf(error, error_len, "missing value for %s", argv[*index]);
    return NULL;
}

static int sirio_cli_set_action(sirio_cli_options *options,
                                sirio_cli_action action,
                                const char *value,
                                char *error, size_t error_len) {
    if (options->command_action_set) {
        snprintf(error, error_len, "only one action may be used with sirio %s",
                 sirio_cli_command_name(options->command));
        return -1;
    }
    options->command_action_set = true;
    options->action = action;
    options->action_value = value;
    return 0;
}

static int sirio_cli_parse(int argc, char **argv,
                           sirio_cli_options *options,
                           char *error, size_t error_len) {
    memset(options, 0, sizeof(*options));
    options->resume_turns = 3;
    options->agent_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!options->agent_argv) {
        snprintf(error, error_len, "out of memory");
        return -1;
    }
    options->agent_argv[options->agent_argc++] = argv[0];

    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        const char *value = NULL;
        sirio_cli_command command = sirio_cli_command_parse(argument);
        if (command != SIRIO_CLI_COMMAND_NONE) {
            if (options->command != SIRIO_CLI_COMMAND_NONE) {
                snprintf(error, error_len,
                         "only one command may be used per invocation");
                return -1;
            }
            options->command = command;
        } else if (options->command == SIRIO_CLI_COMMAND_AUTH &&
                   (!strcmp(argument, "--api-key") ||
                    !strcmp(argument, "--login") ||
                    !strcmp(argument, "--logout"))) {
            value = sirio_cli_value(&i, argc, argv, error, error_len);
            if (!value) return -1;
            sirio_cli_action action = !strcmp(argument, "--api-key") ?
                SIRIO_CLI_API_KEY : !strcmp(argument, "--login") ?
                SIRIO_CLI_LOGIN : SIRIO_CLI_LOGOUT;
            if (sirio_cli_set_action(options, action, value,
                                     error, error_len) != 0)
                return -1;
        } else if (options->command == SIRIO_CLI_COMMAND_AUTH &&
                   !strcmp(argument, "--status")) {
            if (sirio_cli_set_action(options, SIRIO_CLI_AUTH_STATUS, NULL,
                                     error, error_len) != 0)
                return -1;
        } else if (options->command == SIRIO_CLI_COMMAND_SESSIONS &&
                   (!strcmp(argument, "--delete") ||
                    !strcmp(argument, "--strip"))) {
            value = sirio_cli_value(&i, argc, argv, error, error_len);
            if (!value) return -1;
            if (sirio_cli_set_action(options,
                    !strcmp(argument, "--delete") ?
                    SIRIO_CLI_DELETE_SESSION : SIRIO_CLI_STRIP_SESSION,
                    value, error, error_len) != 0)
                return -1;
        } else if (options->command == SIRIO_CLI_COMMAND_SESSIONS &&
                   !strcmp(argument, "--resume")) {
            value = sirio_cli_value(&i, argc, argv, error, error_len);
            if (!value) return -1;
            if (options->command_action_set) {
                snprintf(error, error_len,
                         "only one action may be used with sirio sessions");
                return -1;
            }
            options->resume_session = value;
            options->command_action_set = true;
        } else if (options->command == SIRIO_CLI_COMMAND_SESSIONS &&
                   !strcmp(argument, "--turns")) {
            value = sirio_cli_value(&i, argc, argv, error, error_len);
            if (!value) return -1;
            if (options->resume_turns_set) {
                snprintf(error, error_len, "--turns may be specified once");
                return -1;
            }
            errno = 0;
            char *end = NULL;
            long turns = strtol(value, &end, 10);
            if (errno || !end || *end || turns < 1 || turns > 200) {
                snprintf(error, error_len, "--turns must be between 1 and 200");
                return -1;
            }
            options->resume_turns = (int)turns;
            options->resume_turns_set = true;
        } else if (options->command == SIRIO_CLI_COMMAND_SESSIONS &&
                   !strcmp(argument, "--list")) {
            if (sirio_cli_set_action(options, SIRIO_CLI_SESSIONS, NULL,
                                     error, error_len) != 0)
                return -1;
        } else if (options->command == SIRIO_CLI_COMMAND_CATALOG &&
                   (!strcmp(argument, "--providers") ||
                    !strcmp(argument, "--models"))) {
            sirio_cli_action action = !strcmp(argument, "--providers") ?
                SIRIO_CLI_LIST_PROVIDERS : SIRIO_CLI_LIST_MODELS;
            if (sirio_cli_set_action(options, action, NULL,
                                     error, error_len) != 0)
                return -1;
        } else if (options->command == SIRIO_CLI_COMMAND_CATALOG &&
                   (!strcmp(argument, "--default") ||
                    !strcmp(argument, "--remove"))) {
            value = sirio_cli_value(&i, argc, argv, error, error_len);
            if (!value) return -1;
            sirio_cli_action action = !strcmp(argument, "--default") ?
                SIRIO_CLI_ADD_DEFAULT : SIRIO_CLI_REMOVE_DEFAULT;
            if (sirio_cli_set_action(options, action, value,
                                     error, error_len) != 0)
                return -1;
        } else if (!strcmp(argument, "--provider")) {
            value = sirio_cli_value(&i, argc, argv, error, error_len);
            if (!value) return -1;
            if (options->provider_name) {
                snprintf(error, error_len, "--provider may be specified once");
                return -1;
            }
            options->provider_name = value;
        } else if (!strcmp(argument, "-m") || !strcmp(argument, "--model")) {
            value = sirio_cli_value(&i, argc, argv, error, error_len);
            if (!value) return -1;
            if (options->model_name) {
                snprintf(error, error_len, "--model may be specified once");
                return -1;
            }
            options->model_name = value;
        } else if (options->command == SIRIO_CLI_COMMAND_AUTH &&
                   !strcmp(argument, "--stdin")) {
            options->secret_from_stdin = true;
        } else if ((options->command == SIRIO_CLI_COMMAND_AUTH ||
                    options->command == SIRIO_CLI_COMMAND_SESSIONS) &&
                   (!strcmp(argument, "-y") || !strcmp(argument, "--yes"))) {
            options->assume_yes = true;
        } else if (sirio_agent_option_has_value(argument) ||
                   sirio_agent_option_is_flag(argument)) {
            options->agent_argv[options->agent_argc++] = argv[i];
            if (sirio_agent_option_has_value(argument) && i + 1 < argc)
                options->agent_argv[options->agent_argc++] = argv[++i];
        } else {
            snprintf(error, error_len, "unknown option: %s", argument);
            return -1;
        }
    }

    if (options->secret_from_stdin && options->action != SIRIO_CLI_API_KEY) {
        snprintf(error, error_len, "--stdin requires --api-key");
        return -1;
    }
    if (options->assume_yes && options->action != SIRIO_CLI_API_KEY &&
        options->action != SIRIO_CLI_LOGIN &&
        options->action != SIRIO_CLI_LOGOUT &&
        options->action != SIRIO_CLI_DELETE_SESSION) {
        snprintf(error, error_len,
                 "--yes requires --api-key, --login, --logout, or --delete");
        return -1;
    }
    if (options->resume_turns_set && !options->resume_session) {
        snprintf(error, error_len, "--turns requires --resume");
        return -1;
    }
    if (options->command != SIRIO_CLI_COMMAND_NONE &&
        !options->command_action_set) {
        const char *name = sirio_cli_command_name(options->command);
        snprintf(error, error_len,
                 "%s requires an action; use 'sirio %s --help'",
                 name, name);
        return -1;
    }
    bool resumes = options->command == SIRIO_CLI_COMMAND_SESSIONS &&
                   options->resume_session;
    bool runs = options->command == SIRIO_CLI_COMMAND_NONE || resumes;
    if (options->provider_name &&
        !(options->command == SIRIO_CLI_COMMAND_CATALOG &&
          options->action == SIRIO_CLI_LIST_MODELS)) {
        snprintf(error, error_len,
                 "--provider is only valid with catalog --models");
        return -1;
    }
    if (!runs && options->model_name) {
        snprintf(error, error_len,
                 "--model is only valid for an agent run");
        return -1;
    }
    if (!runs && options->agent_argc != 1) {
        snprintf(error, error_len,
                 "agent options cannot be combined with this action");
        return -1;
    }
    return 0;
}

static void sirio_cli_options_free(sirio_cli_options *options) {
    free(options->agent_argv);
    memset(options, 0, sizeof(*options));
}

static bool sirio_dispatch_help(int argc, char **argv, int *status_out) {
    sirio_cli_command command = SIRIO_CLI_COMMAND_NONE;
    for (int i = 1; i < argc; i++) {
        sirio_cli_command parsed = sirio_cli_command_parse(argv[i]);
        if (parsed != SIRIO_CLI_COMMAND_NONE) {
            command = parsed;
            continue;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            *status_out = sirio_help_print(
                stdout, sirio_cli_command_name(command));
            return true;
        }
        if ((sirio_agent_option_has_value(argv[i]) ||
             !strcmp(argv[i], "--provider") ||
             !strcmp(argv[i], "--model") ||
             !strcmp(argv[i], "--api-key") ||
             !strcmp(argv[i], "--login") ||
             !strcmp(argv[i], "--logout") ||
             !strcmp(argv[i], "--delete") ||
             !strcmp(argv[i], "--strip") ||
             !strcmp(argv[i], "--resume") ||
             !strcmp(argv[i], "--turns") ||
             !strcmp(argv[i], "--default") ||
             !strcmp(argv[i], "--remove")) && i + 1 < argc)
            i++;
    }
    return false;
}

static int sirio_config_path(const char *file, char path[PATH_MAX]) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = ".";
    int length = snprintf(path, PATH_MAX, "%s/.sirio/%s", home, file);
    if (length < 0 || length >= PATH_MAX) {
        sirio_cli_error("home path is too long");
        return -1;
    }
    return 0;
}

static int sirio_auth_path(char path[PATH_MAX]) {
    return sirio_config_path("auth.json", path);
}

static int sirio_defaults_path(char path[PATH_MAX]) {
    return sirio_config_path("default.json", path);
}

static bool sirio_provider_has_auth(const sirio_auth_store *store,
                                    sirio_provider provider) {
    return sirio_auth_store_has(store, provider, SIRIO_AUTH_API_KEY) ||
           sirio_auth_store_has(store, provider, SIRIO_AUTH_OAUTH);
}

static bool sirio_is_subprocess(void) {
    const char *depth = getenv(SIRIO_SUBPROCESS_DEPTH_ENV);
    return depth && depth[0] && strcmp(depth, "0") != 0;
}

static int sirio_resolve_auth(const sirio_auth_store *store,
                              sirio_provider provider,
                              sirio_auth_method *method_out,
                              const char **api_key_out,
                              char *error, size_t error_len) {
    bool api_key = sirio_auth_store_has(
        store, provider, SIRIO_AUTH_API_KEY);
    bool oauth = sirio_auth_store_has(store, provider, SIRIO_AUTH_OAUTH);
    sirio_auth_method preferred = sirio_auth_store_preferred(store, provider);
    sirio_auth_method method = SIRIO_AUTH_NONE;
    if (preferred == SIRIO_AUTH_API_KEY && api_key)
        method = SIRIO_AUTH_API_KEY;
    else if (preferred == SIRIO_AUTH_OAUTH && oauth)
        method = SIRIO_AUTH_OAUTH;
    else if (api_key && !oauth)
        method = SIRIO_AUTH_API_KEY;
    else if (oauth && !api_key)
        method = SIRIO_AUTH_OAUTH;
    else if (api_key && oauth) {
        snprintf(error, error_len,
                 "authentication for %s is ambiguous; authenticate again to select API key or OAuth",
                 sirio_provider_name(provider));
        return -1;
    } else {
        const sirio_provider_info *info = sirio_provider_get(provider);
        snprintf(error, error_len,
                 info && info->supports_oauth ?
                 "no authentication is configured for %s; use 'sirio auth --api-key %s' or 'sirio auth --login %s'" :
                 "no authentication is configured for %s; use 'sirio auth --api-key %s'",
                 info->name, info->name, info->name);
        return -1;
    }
    *method_out = method;
    if (api_key_out)
        *api_key_out = sirio_auth_store_api_key(store, provider);
    return 0;
}

static int sirio_resolve_selection(const sirio_cli_options *options,
                                   const sirio_auth_store *store,
                                   const sirio_default_store *defaults,
                                   bool resume_selection,
                                   sirio_provider *provider_out,
                                   const sirio_model_info **model_out,
                                   char *error, size_t error_len) {
    bool subprocess = sirio_is_subprocess();
    sirio_provider provider = SIRIO_PROVIDER_NONE;
    const sirio_model_info *model = NULL;
    if (options->model_name) {
        model = subprocess || resume_selection ? sirio_model_resolve(
            SIRIO_PROVIDER_NONE, options->model_name, error, error_len) :
            sirio_default_store_resolve(
                defaults, options->model_name, error, error_len);
        if (!model) return 2;
        provider = model->provider;
    } else if (subprocess) {
        snprintf(error, error_len,
                 "subagent process requires an explicit model");
        return 2;
    } else {
        const sirio_model_info *last_model = NULL;
        if (sirio_default_store_last_used(
                defaults, &last_model, NULL) &&
            sirio_provider_has_auth(store, last_model->provider))
            model = last_model;
        size_t count = sirio_default_store_count(defaults);
        for (size_t i = 0; !model && i < count; i++) {
            const sirio_model_info *candidate =
                sirio_default_store_at(defaults, i);
            if (candidate && sirio_provider_has_auth(
                    store, candidate->provider))
                model = candidate;
        }
        if (model) provider = model->provider;
    }
    if (!model) {
        snprintf(error, error_len,
                 "no interface model has configured credentials; use 'sirio catalog --providers' to inspect authentication");
        return 1;
    }
    if (!sirio_provider_has_auth(store, provider)) {
        snprintf(error, error_len,
                 "no authentication is configured for %s; use 'sirio auth --api-key %s'",
                 sirio_provider_name(provider), sirio_provider_name(provider));
        return 1;
    }
    *provider_out = model->provider;
    *model_out = model;
    return 0;
}

typedef struct {
    char auth_path[PATH_MAX];
    char defaults_path[PATH_MAX];
    sirio_default_store *defaults;
    bool subprocess;
    const sirio_model_info *session_model;
} sirio_host_runtime;

static sirio_default_store *sirio_host_load_defaults(
        const sirio_host_runtime *host, char *error, size_t error_len) {
    sirio_default_store *defaults = sirio_default_store_load(
        host->defaults_path, error, error_len);
    if (defaults && sirio_default_store_validate(
            defaults, error, error_len) != 0) {
        sirio_default_store_destroy(defaults);
        return NULL;
    }
    return defaults;
}

static int sirio_host_select_model(sirio_engine *engine,
                                   const char *model_name,
                                   const char *reasoning_name,
                                   char *error, size_t error_len) {
    if (!engine || !engine->select_private_data || !model_name) return 2;
    sirio_host_runtime *host = engine->select_private_data;

    sirio_default_store *defaults = host->subprocess ? NULL :
        sirio_host_load_defaults(host, error, error_len);
    if (!host->subprocess && !defaults) return 1;
    sirio_provider provider_hint = SIRIO_PROVIDER_NONE;
    if (!strchr(model_name, '/') && engine->model &&
        !strcmp(model_name, engine->model->name))
        provider_hint = engine->model->provider;
    char qualified_name[192];
    const char *resolved_name = model_name;
    if (!host->subprocess && provider_hint != SIRIO_PROVIDER_NONE) {
        int length = snprintf(qualified_name, sizeof(qualified_name), "%s/%s",
                              sirio_provider_name(provider_hint), model_name);
        if (length < 0 || (size_t)length >= sizeof(qualified_name)) {
            snprintf(error, error_len, "model selection is too long");
            sirio_default_store_destroy(defaults);
            return 2;
        }
        resolved_name = qualified_name;
    }
    const sirio_model_info *model = host->subprocess ?
        sirio_model_resolve(provider_hint, model_name, error, error_len) :
        sirio_default_store_resolve(
            defaults, resolved_name, error, error_len);
    if (!model && !host->subprocess &&
        (engine->restoring_session || host->session_model)) {
        char catalog_error[256] = {0};
        const sirio_model_info *session_model = sirio_model_resolve(
            provider_hint, resolved_name, catalog_error,
            sizeof(catalog_error));
        if (session_model && (engine->restoring_session ||
                              session_model == host->session_model)) {
            model = session_model;
            error[0] = '\0';
        } else if (engine->restoring_session && catalog_error[0]) {
            snprintf(error, error_len, "%s", catalog_error);
        }
    }
    if (!model) {
        sirio_default_store_destroy(defaults);
        return 2;
    }
    sirio_reasoning_effort reasoning = model == engine->model ?
        engine->reasoning :
        sirio_default_store_reasoning(defaults, model);
    if (reasoning_name &&
        (!sirio_reasoning_parse(reasoning_name, &reasoning) ||
         !sirio_model_supports_reasoning(model, reasoning))) {
        snprintf(error, error_len, "%s does not support reasoning effort %s",
                 model->name, reasoning_name);
        sirio_default_store_destroy(defaults);
        return 2;
    }

    sirio_auth_store *auth = sirio_auth_store_load(
        host->auth_path, error, error_len);
    if (!auth) {
        sirio_default_store_destroy(defaults);
        return 1;
    }
    sirio_auth_method auth_method = SIRIO_AUTH_NONE;
    const char *api_key = NULL;
    if (sirio_resolve_auth(auth, model->provider, &auth_method, &api_key,
                           error, error_len) != 0) {
        sirio_auth_store_destroy(auth);
        sirio_default_store_destroy(defaults);
        return 1;
    }
    sirio_bridge_config config = {
        .api_key = api_key,
        .provider = model->provider,
        .auth_method = auth_method,
        .model = model->name,
        .auth_path = host->auth_path,
    };
    sirio_bridge *bridge = sirio_bridge_create(&config);
    if (!bridge) {
        snprintf(error, error_len, "unable to initialize %s with model %s",
                 sirio_provider_name(model->provider), model->name);
        sirio_auth_store_destroy(auth);
        sirio_default_store_destroy(defaults);
        return 1;
    }
    sirio_generation_options generation = engine->generation;
    generation.reasoning = reasoning;
    if (sirio_bridge_set_generation_options(bridge, &generation) != 0) {
        snprintf(error, error_len, "%s", sirio_bridge_last_error(bridge));
        sirio_bridge_destroy(bridge);
        sirio_auth_store_destroy(auth);
        sirio_default_store_destroy(defaults);
        return 2;
    }
    bool interface_model = !host->subprocess &&
        sirio_default_store_contains(defaults, model);
    if (interface_model &&
        (sirio_default_store_set_last_used(
             defaults, model, reasoning) != 0 ||
         sirio_default_store_save(defaults, host->defaults_path,
                                  error, error_len) != 0)) {
        if (!error[0]) snprintf(error, error_len, "unable to save model selection");
        sirio_bridge_destroy(bridge);
        sirio_auth_store_destroy(auth);
        sirio_default_store_destroy(defaults);
        return 1;
    }

    sirio_auth_store_destroy(auth);

    sirio_bridge *old_bridge = engine->bridge;
    if (old_bridge)
        sirio_bridge_set_cancel_poll(old_bridge, NULL, NULL);
    engine->bridge = bridge;
    engine->provider = model->provider;
    engine->model = model;
    engine->reasoning = reasoning;
    engine->generation = generation;
    if (engine->cancel_poll)
        sirio_bridge_set_cancel_poll(
            bridge, engine->cancel_poll, engine->cancel_poll_private_data);
    if (!host->subprocess) {
        sirio_default_store *old_defaults = host->defaults;
        host->defaults = defaults;
        engine->defaults = defaults;
        sirio_default_store_destroy(old_defaults);
        host->session_model = interface_model ? NULL : model;
    }
    if (old_bridge) sirio_bridge_destroy(old_bridge);
    return 0;
}

static size_t sirio_provider_model_count(sirio_provider provider) {
    size_t count = 0;
    for (size_t i = 0; i < sirio_model_count(); i++)
        if (sirio_model_at(i)->provider == provider) count++;
    return count;
}

static const sirio_model_info *sirio_provider_model_at(
        sirio_provider provider, size_t index) {
    for (size_t i = 0; i < sirio_model_count(); i++) {
        const sirio_model_info *model = sirio_model_at(i);
        if (model->provider != provider) continue;
        if (index--) continue;
        return model;
    }
    return NULL;
}

static int sirio_host_step_model(sirio_engine *engine, int direction,
                                 bool *at_limit,
                                 char *error, size_t error_len) {
    if (at_limit) *at_limit = false;
    if (!engine || !engine->select_private_data || !engine->model ||
        (direction != -1 && direction != 1))
        return 2;
    sirio_host_runtime *host = engine->select_private_data;
    sirio_default_store *defaults = host->subprocess ? NULL :
        sirio_host_load_defaults(host, error, error_len);
    if (!host->subprocess && !defaults) return 1;

    size_t count = host->subprocess ?
        sirio_provider_model_count(engine->provider) :
        sirio_default_store_count(defaults);
    size_t current = count;
    for (size_t i = 0; i < count; i++) {
        const sirio_model_info *candidate = host->subprocess ?
            sirio_provider_model_at(engine->provider, i) :
            sirio_default_store_at(defaults, i);
        if (candidate == engine->model) {
            current = i;
            break;
        }
    }
    if (current == count &&
        (host->subprocess || engine->model != host->session_model)) {
        snprintf(error, error_len, "current model is not available");
        sirio_default_store_destroy(defaults);
        return 2;
    }

    const sirio_model_info *target = NULL;
    sirio_reasoning_effort effort = SIRIO_REASONING_NONE;
    sirio_auth_store *auth = sirio_auth_store_load(
        host->auth_path, error, error_len);
    if (!auth) {
        sirio_default_store_destroy(defaults);
        return 1;
    }
    long first = current < count ? (long)current + direction :
                 direction > 0 ? 0 : (long)count - 1;
    for (long i = first;
         i >= 0 && (size_t)i < count; i += direction) {
        const sirio_model_info *candidate = host->subprocess ?
            sirio_provider_model_at(engine->provider, (size_t)i) :
            sirio_default_store_at(defaults, (size_t)i);
        if (candidate &&
            sirio_provider_has_auth(auth, candidate->provider)) {
            target = candidate;
            effort = candidate->default_reasoning;
            break;
        }
    }
    if (!target) {
        if (at_limit) *at_limit = true;
        sirio_auth_store_destroy(auth);
        sirio_default_store_destroy(defaults);
        return 0;
    }
    char selection[192];
    int length = snprintf(selection, sizeof(selection), "%s/%s",
                          sirio_provider_name(target->provider), target->name);
    char reasoning[16];
    snprintf(reasoning, sizeof(reasoning), "%s",
             sirio_reasoning_name(effort));
    sirio_auth_store_destroy(auth);
    sirio_default_store_destroy(defaults);
    if (length < 0 || (size_t)length >= sizeof(selection)) {
        snprintf(error, error_len, "model selection is too long");
        return 2;
    }
    return sirio_host_select_model(engine, selection, reasoning,
                                   error, error_len);
}

static void sirio_secret_clear(char *secret) {
    if (!secret) return;
    volatile unsigned char *cursor = (volatile unsigned char *)secret;
    size_t length = strlen(secret);
    while (length--) *cursor++ = 0;
    free(secret);
}

static char *sirio_read_line(FILE *fp, bool require_eof,
                             char *error, size_t error_len) {
    char *line = NULL;
    size_t capacity = 0;
    errno = 0;
    ssize_t length = getline(&line, &capacity, fp);
    if (length < 0) {
        snprintf(error, error_len, "cannot read secret: %s",
                 errno ? strerror(errno) : "end of input");
        free(line);
        return NULL;
    }
    if (memchr(line, '\0', (size_t)length) != NULL) {
        snprintf(error, error_len, "secret contains a NUL byte");
        sirio_secret_clear(line);
        return NULL;
    }
    if (length && line[length - 1] == '\n') line[--length] = '\0';
    if (length && line[length - 1] == '\r') line[--length] = '\0';
    if (!length) {
        snprintf(error, error_len, "secret cannot be empty");
        sirio_secret_clear(line);
        return NULL;
    }
    if (require_eof && fgetc(fp) != EOF) {
        snprintf(error, error_len, "--stdin accepts exactly one secret line");
        sirio_secret_clear(line);
        return NULL;
    }
    return line;
}

static char *sirio_read_masked_secret(FILE *tty, int fd,
                                      unsigned char erase,
                                      char *error, size_t error_len) {
    char *secret = NULL;
    size_t length = 0;
    size_t capacity = 0;

    for (;;) {
        unsigned char character;
        ssize_t read_count = read(fd, &character, 1);
        if (read_count < 0) {
            if (errno == EINTR) continue;
            snprintf(error, error_len, "cannot read secret: %s",
                     strerror(errno));
            sirio_secret_clear(secret);
            return NULL;
        }
        if (read_count == 0 || character == 4) {
            if (!length) {
                snprintf(error, error_len, "cannot read secret: end of input");
                sirio_secret_clear(secret);
                return NULL;
            }
            break;
        }
        if (character == '\n' || character == '\r') break;
        if (character == erase || character == 8 || character == 127) {
            if (length) {
                length--;
                fputs("\b \b", tty);
                fflush(tty);
            }
            continue;
        }
        if (character == '\0') {
            snprintf(error, error_len, "secret contains a NUL byte");
            sirio_secret_clear(secret);
            return NULL;
        }
        if (length + 1 >= capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            if (next <= length || next > SIZE_MAX - 1) {
                snprintf(error, error_len, "secret is too long");
                sirio_secret_clear(secret);
                return NULL;
            }
            char *replacement = realloc(secret, next);
            if (!replacement) {
                snprintf(error, error_len, "out of memory");
                sirio_secret_clear(secret);
                return NULL;
            }
            secret = replacement;
            capacity = next;
        }
        secret[length++] = (char)character;
        secret[length] = '\0';
        fputc('*', tty);
        fflush(tty);
    }
    if (!length) {
        snprintf(error, error_len, "secret cannot be empty");
        sirio_secret_clear(secret);
        return NULL;
    }
    secret[length] = '\0';
    return secret;
}

static char *sirio_read_secret(bool from_stdin,
                               const char *provider,
                               char *error, size_t error_len) {
    if (from_stdin) return sirio_read_line(stdin, true, error, error_len);
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) {
        snprintf(error, error_len,
                 "cannot open terminal for secret input; use --stdin");
        return NULL;
    }
    int fd = fileno(tty);
    struct termios original;
    if (tcgetattr(fd, &original) != 0) {
        snprintf(error, error_len, "cannot configure terminal: %s",
                 strerror(errno));
        fclose(tty);
        return NULL;
    }
    struct termios hidden = original;
    hidden.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    hidden.c_cc[VMIN] = 1;
    hidden.c_cc[VTIME] = 0;
    fprintf(tty, "%s API key: ", provider);
    fflush(tty);
    if (tcsetattr(fd, TCSAFLUSH, &hidden) != 0) {
        snprintf(error, error_len, "cannot hide terminal input: %s",
                 strerror(errno));
        fclose(tty);
        return NULL;
    }
    char *secret = sirio_read_masked_secret(
        tty, fd, (unsigned char)original.c_cc[VERASE], error, error_len);
    int restore_status = tcsetattr(fd, TCSAFLUSH, &original);
    fputc('\n', tty);
    fclose(tty);
    if (restore_status != 0) {
        sirio_secret_clear(secret);
        snprintf(error, error_len, "cannot restore terminal input");
        return NULL;
    }
    return secret;
}

static int sirio_confirm(bool assume_yes, const char *message,
                         char *error, size_t error_len) {
    if (assume_yes) return 1;
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) {
        snprintf(error, error_len,
                 "confirmation requires a terminal or --yes");
        return -1;
    }
    fprintf(tty, "%s [y/N] ", message);
    fflush(tty);
    char answer[16] = {0};
    bool read = fgets(answer, sizeof(answer), tty) != NULL;
    fclose(tty);
    if (!read) {
        snprintf(error, error_len, "cannot read confirmation");
        return -1;
    }
    return !strcasecmp(answer, "y\n") || !strcasecmp(answer, "yes\n") ||
           !strcasecmp(answer, "y") || !strcasecmp(answer, "yes");
}

static int sirio_save_store(sirio_auth_store *store, const char *path) {
    char error[256] = {0};
    if (sirio_auth_store_save(store, path, error, sizeof(error)) == 0)
        return 0;
    sirio_cli_error(error[0] ? error : "unable to save authentication");
    return 1;
}

static void sirio_print_auth_method(const sirio_auth_store *store,
                                    sirio_provider provider,
                                    sirio_auth_method method) {
    const char *label = method == SIRIO_AUTH_API_KEY ? "api-key" : "oauth";
    const char *state = sirio_auth_store_has(store, provider, method) ?
                        "saved" : "not configured";
    printf("  %-9s %s", label, state);
    if (sirio_auth_store_preferred(store, provider) == method)
        fputs(" (preferred)", stdout);
    if (method == SIRIO_AUTH_OAUTH) {
        time_t expiry = sirio_auth_store_oauth_expiry(store, provider);
        if (expiry > 0) printf("; expires %lld", (long long)expiry);
    }
    fputc('\n', stdout);
}

typedef struct {
    char **items;
    size_t count;
} sirio_model_arguments;

static void sirio_model_arguments_free(sirio_model_arguments *arguments) {
    if (!arguments) return;
    for (size_t i = 0; i < arguments->count; i++) free(arguments->items[i]);
    free(arguments->items);
    memset(arguments, 0, sizeof(*arguments));
}

static int sirio_model_arguments_parse(const char *value,
                                       sirio_model_arguments *arguments,
                                       char *error, size_t error_len) {
    memset(arguments, 0, sizeof(*arguments));
    char *copy = value ? strdup(value) : NULL;
    if (!copy) {
        snprintf(error, error_len, value ? "out of memory" :
                 "model list cannot be empty");
        return -1;
    }
    char *cursor = copy;
    for (;;) {
        char *comma = strchr(cursor, ',');
        if (comma) *comma = '\0';
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        char *end = cursor + strlen(cursor);
        while (end > cursor && isspace((unsigned char)end[-1])) end--;
        *end = '\0';
        if (!cursor[0]) {
            snprintf(error, error_len, "model list contains an empty entry");
            free(copy);
            sirio_model_arguments_free(arguments);
            return -1;
        }
        char *item = strdup(cursor);
        if (!item) {
            free(copy);
            sirio_model_arguments_free(arguments);
            snprintf(error, error_len, "out of memory");
            return -1;
        }
        char **items = realloc(
            arguments->items,
            (arguments->count + 1) * sizeof(*arguments->items));
        if (!items) {
            free(item);
            free(copy);
            sirio_model_arguments_free(arguments);
            snprintf(error, error_len, "out of memory");
            return -1;
        }
        arguments->items = items;
        arguments->items[arguments->count++] = item;
        if (!comma) break;
        cursor = comma + 1;
    }
    free(copy);
    return 0;
}

static bool sirio_model_reference_syntax(const char *reference) {
    if (!reference || !reference[0]) return false;
    const char *slash = strchr(reference, '/');
    if (!slash || slash == reference || !slash[1] || strchr(slash + 1, '/'))
        return false;
    for (const unsigned char *cursor =
             (const unsigned char *)reference; *cursor; cursor++)
        if (isspace(*cursor)) return false;
    return true;
}

static int sirio_run_model_action(const sirio_cli_options *options,
                                  sirio_default_store *defaults,
                                  const char *defaults_path) {
    char error[256] = {0};
    if (options->action == SIRIO_CLI_LIST_MODELS) {
        if (sirio_default_store_validate(
                defaults, error, sizeof(error)) != 0) {
            sirio_cli_error(error);
            return 1;
        }
        const sirio_provider_info *filter = options->provider_name ?
            sirio_provider_find(options->provider_name) : NULL;
        if (options->provider_name && !filter) {
            fprintf(stderr, "sirio: unknown provider: %s\n",
                    options->provider_name);
            return 2;
        }
        for (size_t i = 0; i < sirio_model_count(); i++) {
            const sirio_model_info *model = sirio_model_at(i);
            if (filter && model->provider != filter->id) continue;
            char reference[192];
            int length = snprintf(reference, sizeof(reference), "%s/%s",
                                  sirio_provider_name(model->provider),
                                  model->name);
            if (length <= 0 || (size_t)length >= sizeof(reference)) {
                sirio_cli_error("model reference is too long");
                return 1;
            }
            printf("%-42s scope %s\n", reference,
                   sirio_default_store_contains(defaults, model) ?
                   "interface" : "subagent");
        }
        return 0;
    }

    sirio_model_arguments arguments;
    if (sirio_model_arguments_parse(
            options->action_value, &arguments,
            error, sizeof(error)) != 0) {
        sirio_cli_error(error);
        return strstr(error, "memory") ? 1 : 2;
    }
    bool adds = options->action == SIRIO_CLI_ADD_DEFAULT;
    if (adds && sirio_default_store_validate(
            defaults, error, sizeof(error)) != 0) {
        sirio_cli_error(error);
        sirio_model_arguments_free(&arguments);
        return 1;
    }
    for (size_t i = 0; i < arguments.count; i++) {
        const char *reference = arguments.items[i];
        if (!sirio_model_reference_syntax(reference)) {
            snprintf(error, sizeof(error),
                     "model must use provider/model syntax: %s", reference);
            sirio_cli_error(error);
            sirio_model_arguments_free(&arguments);
            return 2;
        }
        if (adds && !sirio_model_resolve(
                SIRIO_PROVIDER_NONE, reference,
                error, sizeof(error))) {
            sirio_cli_error(error);
            sirio_model_arguments_free(&arguments);
            return 2;
        }
    }

    size_t changed_count = 0;
    for (size_t i = 0; i < arguments.count; i++) {
        bool changed = false;
        int status;
        if (adds) {
            const sirio_model_info *model = sirio_model_resolve(
                SIRIO_PROVIDER_NONE, arguments.items[i],
                error, sizeof(error));
            status = sirio_default_store_add(defaults, model, &changed);
        } else {
            status = sirio_default_store_remove(
                defaults, arguments.items[i], &changed);
        }
        if (status != 0) {
            sirio_cli_error("unable to update interface models");
            sirio_model_arguments_free(&arguments);
            return 1;
        }
        if (changed) changed_count++;
    }
    sirio_model_arguments_free(&arguments);
    if (changed_count && sirio_default_store_save(
            defaults, defaults_path, error, sizeof(error)) != 0) {
        sirio_cli_error(error);
        return 1;
    }
    if (!changed_count) {
        fputs("Interface models unchanged.\n", stdout);
    } else {
        printf("%s %zu interface model%s.\n",
               adds ? "Added" : "Removed", changed_count,
               changed_count == 1 ? "" : "s");
    }
    return 0;
}

static int sirio_run_action(const sirio_cli_options *options,
                            sirio_auth_store *store,
                            const char *auth_path) {
    char error[256] = {0};
    if (options->action == SIRIO_CLI_LIST_PROVIDERS) {
        for (size_t i = 0; i < sirio_provider_count(); i++) {
            const sirio_provider_info *info = sirio_provider_at(i);
            printf("%-10s auth ", info->name);
            if (info->supports_api_key) fputs("api-key", stdout);
            if (info->supports_api_key && info->supports_oauth) fputc('/', stdout);
            if (info->supports_oauth) fputs("oauth", stdout);
            printf("; credentials %s\n",
                   sirio_provider_has_auth(store, info->id) ?
                   "available" : "not configured");
        }
        return 0;
    }
    if (options->action == SIRIO_CLI_AUTH_STATUS) {
        for (size_t i = 0; i < sirio_provider_count(); i++) {
            const sirio_provider_info *info = sirio_provider_at(i);
            printf("%s\n", info->name);
            if (info->supports_api_key)
                sirio_print_auth_method(store, info->id,
                                        SIRIO_AUTH_API_KEY);
            if (info->supports_oauth)
                sirio_print_auth_method(store, info->id, SIRIO_AUTH_OAUTH);
        }
        return 0;
    }

    const sirio_provider_info *info = sirio_provider_find(
        options->action_value);
    if (!info) {
        fprintf(stderr, "sirio: unknown provider: %s\n",
                options->action_value);
        return 2;
    }
    if (options->action == SIRIO_CLI_API_KEY) {
        if (!info->supports_api_key) {
            fprintf(stderr, "sirio: %s does not support API-key authentication\n",
                    info->name);
            return 2;
        }
        if (sirio_auth_store_has(store, info->id, SIRIO_AUTH_API_KEY)) {
            char prompt[128];
            snprintf(prompt, sizeof(prompt),
                     "Replace the saved %s API key?", info->name);
            int confirmed = sirio_confirm(options->assume_yes, prompt,
                                          error, sizeof(error));
            if (confirmed <= 0) {
                if (confirmed < 0) sirio_cli_error(error);
                else fputs("Authentication unchanged.\n", stdout);
                return confirmed < 0 ? 1 : 0;
            }
        }
        char *secret = sirio_read_secret(options->secret_from_stdin,
                                         info->name,
                                         error, sizeof(error));
        if (!secret) {
            sirio_cli_error(error);
            return 1;
        }
        int set_result = sirio_auth_store_set_api_key(
            store, info->id, secret);
        sirio_secret_clear(secret);
        if (set_result != 0 || sirio_auth_store_set_preferred(
                store, info->id, SIRIO_AUTH_API_KEY) != 0) {
            sirio_cli_error("unable to store API key");
            return 1;
        }
        if (sirio_save_store(store, auth_path) != 0) return 1;
        printf("Saved API-key authentication for %s.\n", info->name);
        return 0;
    }
    if (options->action == SIRIO_CLI_LOGOUT) {
        bool saved = sirio_auth_store_has(store, info->id,
                                          SIRIO_AUTH_API_KEY) ||
                     sirio_auth_store_has(store, info->id, SIRIO_AUTH_OAUTH);
        if (!saved) {
            printf("No saved credentials for %s.\n", info->name);
            return 0;
        }
        char prompt[128];
        snprintf(prompt, sizeof(prompt),
                 "Remove all saved credentials for %s?", info->name);
        int confirmed = sirio_confirm(options->assume_yes, prompt,
                                      error, sizeof(error));
        if (confirmed <= 0) {
            if (confirmed < 0) sirio_cli_error(error);
            else fputs("Authentication unchanged.\n", stdout);
            return confirmed < 0 ? 1 : 0;
        }
        sirio_auth_store_clear_provider(store, info->id);
        if (sirio_save_store(store, auth_path) != 0) return 1;
        printf("Removed saved credentials for %s.\n", info->name);
        return 0;
    }
    if (options->action == SIRIO_CLI_LOGIN) {
        if (!info->supports_oauth) {
            fprintf(stderr, "sirio: %s does not support OAuth login\n",
                    info->name);
            return 2;
        }
        if (sirio_auth_store_has(store, info->id, SIRIO_AUTH_OAUTH)) {
            char prompt[128];
            snprintf(prompt, sizeof(prompt),
                     "Replace the saved %s OAuth login?", info->name);
            int confirmed = sirio_confirm(options->assume_yes, prompt,
                                          error, sizeof(error));
            if (confirmed <= 0) {
                if (confirmed < 0) sirio_cli_error(error);
                else fputs("Authentication unchanged.\n", stdout);
                return confirmed < 0 ? 1 : 0;
            }
        }
        sirio_bridge_config config = {
            .provider = info->id,
            .auth_method = SIRIO_AUTH_OAUTH,
            .model = info->default_model,
            .auth_path = auth_path,
        };
        sirio_bridge *bridge = sirio_bridge_create(&config);
        if (!bridge) {
            sirio_cli_error("unable to initialize OAuth login");
            return 1;
        }
        int result = sirio_bridge_authenticate(bridge);
        if (result != 0)
            fprintf(stderr, "sirio: %s\n", sirio_bridge_last_error(bridge));
        sirio_bridge_destroy(bridge);
        if (result != 0) return 1;
        printf("Saved OAuth authentication for %s.\n", info->name);
        return 0;
    }
    sirio_cli_error("unsupported action");
    return 2;
}

int sirio_main(int argc, char **argv) {
    int help_status = 0;
    if (sirio_dispatch_help(argc, argv, &help_status)) return help_status;

    char error[256] = {0};
    sirio_cli_options options;
    if (sirio_cli_parse(argc, argv, &options, error, sizeof(error)) != 0) {
        sirio_cli_error(error);
        sirio_cli_options_free(&options);
        return 2;
    }

    if (options.action == SIRIO_CLI_SESSIONS) {
        int result = sirio_sessions_list(stdout, error, sizeof(error));
        if (result != 0) sirio_cli_error(error);
        sirio_cli_options_free(&options);
        return result;
    }
    if (options.action == SIRIO_CLI_DELETE_SESSION) {
        sirio_session_info info;
        if (sirio_session_inspect(options.action_value, &info,
                                  error, sizeof(error)) != 0) {
            sirio_cli_error(error);
            sirio_cli_options_free(&options);
            return 1;
        }
        char prompt[512];
        snprintf(prompt, sizeof(prompt), "Delete session %.8s (%s)?",
                 info.sha, info.title);
        int confirmed = sirio_confirm(options.assume_yes, prompt,
                                      error, sizeof(error));
        if (confirmed <= 0) {
            if (confirmed < 0) sirio_cli_error(error);
            else fputs("Session unchanged.\n", stdout);
            sirio_cli_options_free(&options);
            return confirmed < 0 ? 1 : 0;
        }
        if (sirio_session_delete(info.sha, &info,
                                 error, sizeof(error)) != 0) {
            sirio_cli_error(error);
            sirio_cli_options_free(&options);
            return 1;
        }
        printf("Deleted session %.8s.\n", info.sha);
        sirio_cli_options_free(&options);
        return 0;
    }
    if (options.action == SIRIO_CLI_STRIP_SESSION) {
        sirio_session_info info;
        uint32_t tokens = 0;
        int result = sirio_session_strip(options.action_value, &info, &tokens,
                                         error, sizeof(error));
        if (result != 0) sirio_cli_error(error);
        else printf("Rewritten session %.8s (%u tokens).\n",
                    info.sha, tokens);
        sirio_cli_options_free(&options);
        return result;
    }

    agent_config run_config;
    if (options.action == SIRIO_CLI_RUN &&
        sirio_agent_parse_options(&run_config, options.agent_argc,
                                  options.agent_argv,
                                  error, sizeof(error)) != 0) {
        sirio_cli_error(error[0] ? error : "invalid agent options");
        sirio_cli_options_free(&options);
        return 2;
    }
    sirio_session_info resume_info = {0};
    bool resume_saved_reasoning = false;
    if (options.resume_session) {
        if (run_config.gen.prompt || run_config.non_interactive ||
            run_config.gen.raw_prompt || run_config.gen.system_set) {
            sirio_cli_error(
                "--resume is interactive and cannot be combined with --prompt, --non-interactive, --raw-prompt, or --system");
            sirio_cli_options_free(&options);
            return 2;
        }
        if (sirio_session_inspect(options.resume_session, &resume_info,
                                  error, sizeof(error)) != 0) {
            sirio_cli_error(error);
            sirio_cli_options_free(&options);
            return 1;
        }
        run_config.resume_session = resume_info.sha;
        run_config.resume_history_turns = options.resume_turns;
        if (!options.provider_name && !options.model_name &&
            !run_config.gen.reasoning_set) {
            sirio_reasoning_effort effort;
            if (!sirio_reasoning_parse(resume_info.reasoning, &effort)) {
                sirio_cli_error("saved session has an invalid reasoning effort");
                sirio_cli_options_free(&options);
                return 1;
            }
            run_config.gen.think_mode = (sirio_think_mode)effort;
            run_config.gen.reasoning_set = true;
            resume_saved_reasoning = true;
        }
    }
    char executable_path[PATH_MAX] = {0};
    if (options.action == SIRIO_CLI_RUN &&
        sirio_executable_path(argv[0], executable_path))
        run_config.executable_path = executable_path;

    bool model_action = options.action == SIRIO_CLI_LIST_MODELS ||
                        options.action == SIRIO_CLI_ADD_DEFAULT ||
                        options.action == SIRIO_CLI_REMOVE_DEFAULT;
    char defaults_path[PATH_MAX] = {0};
    if (model_action) {
        if (sirio_defaults_path(defaults_path) != 0) {
            sirio_cli_options_free(&options);
            return 1;
        }
        sirio_default_store *defaults = sirio_default_store_load(
            defaults_path, error, sizeof(error));
        if (!defaults) {
            sirio_cli_error(error[0] ? error :
                            "unable to load interface models");
            sirio_cli_options_free(&options);
            return 1;
        }
        int result = sirio_run_model_action(
            &options, defaults, defaults_path);
        sirio_default_store_destroy(defaults);
        sirio_cli_options_free(&options);
        return result;
    }

    char auth_path[PATH_MAX];
    if (sirio_auth_path(auth_path) != 0) {
        sirio_cli_options_free(&options);
        return 1;
    }
    sirio_auth_store *store = sirio_auth_store_load(
        auth_path, error, sizeof(error));
    if (!store) {
        sirio_cli_error(error[0] ? error : "unable to load authentication");
        sirio_cli_options_free(&options);
        return 1;
    }
    if (options.action != SIRIO_CLI_RUN) {
        int result = sirio_run_action(&options, store, auth_path);
        sirio_auth_store_destroy(store);
        sirio_cli_options_free(&options);
        return result;
    }

    bool subprocess = sirio_is_subprocess();
    sirio_default_store *defaults = NULL;
    if (!subprocess) {
        if (sirio_defaults_path(defaults_path) != 0) {
            sirio_auth_store_destroy(store);
            sirio_cli_options_free(&options);
            return 1;
        }
        defaults = sirio_default_store_load(
            defaults_path, error, sizeof(error));
        if (!defaults || sirio_default_store_validate(
                defaults, error, sizeof(error)) != 0) {
            sirio_cli_error(error[0] ? error :
                            "unable to load interface models");
            sirio_default_store_destroy(defaults);
            sirio_auth_store_destroy(store);
            sirio_cli_options_free(&options);
            return 1;
        }
    }

    bool resumes_saved_model = options.resume_session && !options.model_name;
    if (!subprocess && !resumes_saved_model &&
        sirio_default_store_count(defaults) == 0) {
        sirio_cli_error(
            "no interface models are configured; use 'sirio catalog --default provider/model[,provider/model...]'");
        sirio_default_store_destroy(defaults);
        sirio_auth_store_destroy(store);
        sirio_cli_options_free(&options);
        return 1;
    }

    sirio_provider provider;
    const sirio_model_info *model = NULL;
    sirio_cli_options selection_options = options;
    char resume_selection[192];
    if (options.resume_session && !options.provider_name &&
        !options.model_name) {
        selection_options.provider_name = resume_info.provider;
        selection_options.model_name = resume_info.model;
        int length = snprintf(resume_selection, sizeof(resume_selection),
                              "%s/%s", resume_info.provider,
                              resume_info.model);
        if (length < 0 || (size_t)length >= sizeof(resume_selection)) {
            sirio_cli_error("saved model selection is too long");
            sirio_default_store_destroy(defaults);
            sirio_auth_store_destroy(store);
            sirio_cli_options_free(&options);
            return 1;
        }
        selection_options.model_name = resume_selection;
    }
    int selection_status = sirio_resolve_selection(
        &selection_options, store, defaults, resumes_saved_model,
        &provider, &model,
        error, sizeof(error));
    if (selection_status != 0) {
        int result = resumes_saved_model ? 1 : selection_status;
        sirio_cli_error(error);
        sirio_default_store_destroy(defaults);
        sirio_auth_store_destroy(store);
        sirio_cli_options_free(&options);
        return result;
    }
    if (resume_saved_reasoning && !sirio_model_supports_reasoning(
            model, (sirio_reasoning_effort)run_config.gen.think_mode)) {
        sirio_cli_error("saved reasoning effort is not supported by its model");
        sirio_default_store_destroy(defaults);
        sirio_auth_store_destroy(store);
        sirio_cli_options_free(&options);
        return 1;
    }
    sirio_host_runtime host = {
        .defaults = defaults,
        .subprocess = subprocess,
        .session_model = resumes_saved_model &&
            !sirio_default_store_contains(defaults, model) ? model : NULL,
    };
    snprintf(host.auth_path, sizeof(host.auth_path), "%s", auth_path);
    snprintf(host.defaults_path, sizeof(host.defaults_path), "%s",
             defaults_path);
    sirio_reasoning_effort initial_reasoning =
        sirio_default_store_reasoning(defaults, model);
    if (resume_saved_reasoning)
        initial_reasoning =
            (sirio_reasoning_effort)run_config.gen.think_mode;
    sirio_engine engine = {
        .provider = provider,
        .model = model,
        .defaults = defaults,
        .reasoning = initial_reasoning,
        .select = sirio_host_select_model,
        .step_model = sirio_host_step_model,
        .select_private_data = &host,
    };
    int result = sirio_agent_run(&engine, &run_config);
    if (engine.bridge) sirio_bridge_destroy(engine.bridge);
    sirio_default_store_destroy(host.defaults);
    sirio_auth_store_destroy(store);
    sirio_cli_options_free(&options);
    return result;
}
#ifndef SIRIO_NO_MAIN
int main(int argc, char **argv) {
    return sirio_main(argc, argv);
}
#endif
