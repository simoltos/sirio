#include "sirio_container.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} protocol_buffer;

typedef struct {
    const char *cur;
    const char *end;
    char error[160];
} protocol_parser;

static int protocol_buffer_append(protocol_buffer *buffer,
                                  const char *data, size_t len) {
    if (len > SIZE_MAX - buffer->len - 1) return -1;
    size_t needed = buffer->len + len + 1;
    if (needed > buffer->cap) {
        size_t cap = buffer->cap ? buffer->cap : 256;
        while (cap < needed) {
            if (cap > SIZE_MAX / 2) {
                cap = needed;
                break;
            }
            cap *= 2;
        }
        char *replacement = realloc(buffer->data, cap);
        if (!replacement) return -1;
        buffer->data = replacement;
        buffer->cap = cap;
    }
    if (len) memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return 0;
}

static int protocol_buffer_puts(protocol_buffer *buffer, const char *text) {
    return protocol_buffer_append(buffer, text, strlen(text));
}

static int protocol_buffer_putc(protocol_buffer *buffer, char byte) {
    return protocol_buffer_append(buffer, &byte, 1);
}

static char *protocol_buffer_take(protocol_buffer *buffer) {
    if (!buffer->data) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    char *data = buffer->data;
    memset(buffer, 0, sizeof(*buffer));
    return data;
}

static int protocol_json_string(protocol_buffer *buffer, const char *text) {
    if (!text) text = "";
    if (protocol_buffer_putc(buffer, '"') != 0) return -1;
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
            if (protocol_buffer_puts(buffer, replacement) != 0) return -1;
        } else if (protocol_buffer_append(
                       buffer, (const char *)cursor, 1) != 0) {
            return -1;
        }
    }
    return protocol_buffer_putc(buffer, '"');
}

static void protocol_skip_space(protocol_parser *parser) {
    while (parser->cur < parser->end &&
           isspace((unsigned char)*parser->cur))
        parser->cur++;
}

static int protocol_fail(protocol_parser *parser, const char *message) {
    if (!parser->error[0])
        snprintf(parser->error, sizeof(parser->error), "%s", message);
    return -1;
}

static int protocol_expect(protocol_parser *parser, char expected) {
    protocol_skip_space(parser);
    if (parser->cur >= parser->end || *parser->cur != expected)
        return protocol_fail(parser, "unexpected JSON token");
    parser->cur++;
    return 0;
}

static int protocol_hex(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static int protocol_codepoint(protocol_buffer *buffer, uint32_t value) {
    char bytes[4];
    size_t len;
    if (value <= 0x7f) {
        bytes[0] = (char)value;
        len = 1;
    } else if (value <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (value >> 6));
        bytes[1] = (char)(0x80 | (value & 0x3f));
        len = 2;
    } else if (value <= 0xffff) {
        bytes[0] = (char)(0xe0 | (value >> 12));
        bytes[1] = (char)(0x80 | ((value >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (value & 0x3f));
        len = 3;
    } else if (value <= 0x10ffff) {
        bytes[0] = (char)(0xf0 | (value >> 18));
        bytes[1] = (char)(0x80 | ((value >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((value >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (value & 0x3f));
        len = 4;
    } else {
        return -1;
    }
    return protocol_buffer_append(buffer, bytes, len);
}

static int protocol_hex4(protocol_parser *parser, uint32_t *value) {
    if ((size_t)(parser->end - parser->cur) < 4)
        return protocol_fail(parser, "truncated Unicode escape");
    uint32_t parsed = 0;
    for (int i = 0; i < 4; i++) {
        int digit = protocol_hex(parser->cur[i]);
        if (digit < 0)
            return protocol_fail(parser, "invalid Unicode escape");
        parsed = parsed * 16 + (uint32_t)digit;
    }
    parser->cur += 4;
    *value = parsed;
    return 0;
}

static int protocol_parse_string(protocol_parser *parser, char **out) {
    protocol_skip_space(parser);
    if (parser->cur >= parser->end || *parser->cur != '"')
        return protocol_fail(parser, "expected JSON string");
    parser->cur++;
    protocol_buffer decoded = {0};
    while (parser->cur < parser->end) {
        unsigned char byte = (unsigned char)*parser->cur++;
        if (byte == '"') {
            *out = protocol_buffer_take(&decoded);
            if (!*out) return protocol_fail(parser, "out of memory");
            return 0;
        }
        if (byte < 0x20) {
            free(decoded.data);
            return protocol_fail(parser, "control byte in JSON string");
        }
        if (byte != '\\') {
            if (protocol_buffer_append(
                    &decoded, (const char *)&byte, 1) != 0) {
                free(decoded.data);
                return protocol_fail(parser, "out of memory");
            }
            continue;
        }
        if (parser->cur >= parser->end) {
            free(decoded.data);
            return protocol_fail(parser, "truncated JSON escape");
        }
        char escape = *parser->cur++;
        char replacement;
        switch (escape) {
        case '"': replacement = '"'; break;
        case '\\': replacement = '\\'; break;
        case '/': replacement = '/'; break;
        case 'b': replacement = '\b'; break;
        case 'f': replacement = '\f'; break;
        case 'n': replacement = '\n'; break;
        case 'r': replacement = '\r'; break;
        case 't': replacement = '\t'; break;
        case 'u': {
            uint32_t codepoint;
            if (protocol_hex4(parser, &codepoint) != 0) {
                free(decoded.data);
                return -1;
            }
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if ((size_t)(parser->end - parser->cur) < 6 ||
                    parser->cur[0] != '\\' || parser->cur[1] != 'u') {
                    free(decoded.data);
                    return protocol_fail(parser, "missing low surrogate");
                }
                parser->cur += 2;
                uint32_t low;
                if (protocol_hex4(parser, &low) != 0) {
                    free(decoded.data);
                    return -1;
                }
                if (low < 0xdc00 || low > 0xdfff) {
                    free(decoded.data);
                    return protocol_fail(parser, "invalid low surrogate");
                }
                codepoint = 0x10000 +
                    ((codepoint - 0xd800) << 10) + (low - 0xdc00);
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                free(decoded.data);
                return protocol_fail(parser, "unexpected low surrogate");
            }
            if (codepoint == 0 ||
                protocol_codepoint(&decoded, codepoint) != 0) {
                free(decoded.data);
                return protocol_fail(parser, codepoint == 0 ?
                                     "NUL in JSON string" : "out of memory");
            }
            continue;
        }
        default:
            free(decoded.data);
            return protocol_fail(parser, "invalid JSON escape");
        }
        if (protocol_buffer_append(&decoded, &replacement, 1) != 0) {
            free(decoded.data);
            return protocol_fail(parser, "out of memory");
        }
    }
    free(decoded.data);
    return protocol_fail(parser, "unterminated JSON string");
}

static void protocol_copy_error(char *error, size_t error_len,
                                const char *message) {
    if (error && error_len)
        snprintf(error, error_len, "%s", message && message[0] ? message :
                 "invalid tool protocol message");
}

static char *sirio_container_encode_request(
        const char *tool, const sirio_container_argument *arguments,
        size_t argument_count) {
    if (!tool || (!arguments && argument_count)) return NULL;
    protocol_buffer buffer = {0};
    if (protocol_buffer_puts(&buffer, "{\"tool\":") != 0 ||
        protocol_json_string(&buffer, tool) != 0 ||
        protocol_buffer_puts(&buffer, ",\"args\":{") != 0)
        goto fail;
    for (size_t i = 0; i < argument_count; i++) {
        if (!arguments[i].name || !arguments[i].value) goto fail;
        if ((i && protocol_buffer_putc(&buffer, ',') != 0) ||
            protocol_json_string(&buffer, arguments[i].name) != 0 ||
            protocol_buffer_putc(&buffer, ':') != 0)
            goto fail;
        if (arguments[i].is_string) {
            if (protocol_json_string(&buffer, arguments[i].value) != 0)
                goto fail;
        } else if (protocol_buffer_puts(&buffer, arguments[i].value) != 0) {
            goto fail;
        }
    }
    if (protocol_buffer_puts(&buffer, "}}") != 0) goto fail;
    return protocol_buffer_take(&buffer);

fail:
    free(buffer.data);
    return NULL;
}

static int sirio_container_decode_response(const char *json, char **text,
                                           bool *is_error,
                                           char *error, size_t error_len) {
    if (text) *text = NULL;
    if (!json || !text || !is_error) {
        protocol_copy_error(error, error_len, "missing response");
        return -1;
    }
    protocol_parser parser = {.cur = json, .end = json + strlen(json)};
    char *key = NULL;
    if (protocol_expect(&parser, '{') != 0 ||
        protocol_parse_string(&parser, &key) != 0 ||
        protocol_expect(&parser, ':') != 0 ||
        protocol_parse_string(&parser, text) != 0 ||
        protocol_expect(&parser, '}') != 0)
        goto fail;
    if (!strcmp(key, "result")) *is_error = false;
    else if (!strcmp(key, "error")) *is_error = true;
    else {
        protocol_fail(&parser, "unknown response field");
        goto fail;
    }
    protocol_skip_space(&parser);
    if (parser.cur != parser.end) {
        protocol_fail(&parser, "trailing response data");
        goto fail;
    }
    free(key);
    return 0;

fail:
    protocol_copy_error(error, error_len, parser.error);
    free(key);
    free(*text);
    *text = NULL;
    return -1;
}


struct sirio_container {
    char command[PATH_MAX];
    char name[96];
    pid_t exec_pid;
    int input_fd;
    int output_fd;
    char *read_buffer;
    size_t read_len;
    size_t read_cap;
    bool started;
    bool ready;
};

static void *container_xmalloc(size_t size) {
    void *pointer = malloc(size ? size : 1);
    if (!pointer) {
        perror("sirio: malloc");
        abort();
    }
    return pointer;
}

static void *container_xrealloc(void *pointer, size_t size) {
    void *replacement = realloc(pointer, size ? size : 1);
    if (!replacement) {
        perror("sirio: realloc");
        abort();
    }
    return replacement;
}

static char *container_xstrdup(const char *text) {
    if (!text) text = "";
    size_t length = strlen(text) + 1;
    char *copy = container_xmalloc(length);
    memcpy(copy, text, length);
    return copy;
}

static char *container_xstrndup(const char *text, size_t length) {
    char *copy = container_xmalloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static bool container_command_exists(const char *name) {
    if (!name || !name[0]) return false;
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path_env = getenv("PATH");
    if (!path_env) return false;
    char *path = container_xstrdup(path_env);
    char *save = NULL;
    bool found = false;
    for (char *directory = strtok_r(path, ":", &save); directory;
         directory = strtok_r(NULL, ":", &save)) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s",
                 directory[0] ? directory : ".", name);
        if (access(candidate, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(path);
    return found;
}

static int container_wait(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static int container_command(char *const argv[], bool quiet_stdout) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (quiet_stdout) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                if (null_fd != STDOUT_FILENO) close(null_fd);
            }
        }
        execvp(argv[0], argv);
        fprintf(stderr, "sirio: cannot execute %s: %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }
    return container_wait(pid);
}

static bool container_write_all(int fd, const char *data, size_t length) {
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written;
        length -= (size_t)written;
    }
    return true;
}

static int container_read_line(sirio_container *container, char **line,
                               char *error, size_t error_len) {
    *line = NULL;
    for (;;) {
        char *newline = container->read_buffer ?
            memchr(container->read_buffer, '\n', container->read_len) : NULL;
        if (newline) {
            size_t length = (size_t)(newline - container->read_buffer);
            if (length && container->read_buffer[length - 1] == '\r')
                length--;
            *line = container_xstrndup(container->read_buffer, length);
            size_t consumed =
                (size_t)(newline - container->read_buffer) + 1;
            memmove(container->read_buffer,
                    container->read_buffer + consumed,
                    container->read_len - consumed);
            container->read_len -= consumed;
            container->read_buffer[container->read_len] = '\0';
            return 0;
        }
        if (container->read_len + 4097 > container->read_cap) {
            size_t capacity = container->read_cap ?
                container->read_cap * 2 : 8192;
            while (capacity < container->read_len + 4097) capacity *= 2;
            container->read_buffer = container_xrealloc(
                container->read_buffer, capacity);
            container->read_cap = capacity;
        }
        ssize_t count = read(container->output_fd,
                             container->read_buffer + container->read_len,
                             4096);
        if (count > 0) {
            container->read_len += (size_t)count;
            container->read_buffer[container->read_len] = '\0';
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0)
            snprintf(error, error_len, "tool runner closed its output");
        else
            snprintf(error, error_len, "tool runner read failed: %s",
                     strerror(errno));
        return -1;
    }
}

int sirio_container_call(sirio_container *container, const char *tool,
                         const sirio_container_argument *arguments,
                         size_t argument_count, char **result,
                         char *error, size_t error_len) {
    if (result) *result = NULL;
    if (!container || !container->ready || !tool || !result) {
        snprintf(error, error_len, "tool runner is unavailable");
        return -1;
    }
    char *request = sirio_container_encode_request(
        tool, arguments, argument_count);
    if (!request) {
        snprintf(error, error_len, "failed to encode tool request");
        return -1;
    }
    bool written = container_write_all(
        container->input_fd, request, strlen(request)) &&
        container_write_all(container->input_fd, "\n", 1);
    free(request);
    if (!written) {
        snprintf(error, error_len, "tool runner write failed: %s",
                 strerror(errno));
        return -1;
    }
    char *response = NULL;
    if (container_read_line(container, &response, error, error_len) != 0)
        return -1;
    bool is_error = false;
    char *text = NULL;
    int status = sirio_container_decode_response(
        response, &text, &is_error, error, error_len);
    free(response);
    if (status != 0) return -1;
    if (is_error) {
        snprintf(error, error_len, "%s",
                 text && text[0] ? text : "tool runner rejected the request");
        free(text);
        return -1;
    }
    *result = text;
    return 0;
}

void sirio_container_stop(sirio_container *container) {
    if (!container) return;
    if (container->ready && container->input_fd >= 0 &&
        container->output_fd >= 0) {
        char error[160] = {0};
        char *reply = NULL;
        (void)sirio_container_call(container, "shutdown", NULL, 0,
                                   &reply, error, sizeof(error));
        free(reply);
    }
    if (container->input_fd >= 0) close(container->input_fd);
    if (container->output_fd >= 0) close(container->output_fd);
    container->input_fd = container->output_fd = -1;
    if (container->exec_pid > 0) {
        (void)container_wait(container->exec_pid);
        container->exec_pid = 0;
    }
    if (container->started) {
        char *argv[] = {container->command, "stop", "-t", "0",
                        container->name, NULL};
        (void)container_command(argv, true);
        container->started = false;
    }
    free(container->read_buffer);
    free(container);
}

sirio_container *sirio_container_start(
        const sirio_container_options *options,
        char *error, size_t error_len) {
    sirio_container *container = container_xmalloc(sizeof(*container));
    memset(container, 0, sizeof(*container));
    container->input_fd = -1;
    container->output_fd = -1;
    if (!container_command_exists("podman")) {
        snprintf(error, error_len, "podman is not available");
        goto fail;
    }
    snprintf(container->command, sizeof(container->command), "podman");
    snprintf(container->name, sizeof(container->name),
             "cma-runtime-%ld", (long)getpid());

    char *image_argv[] = {
        container->command, "image", "exists", "cma", NULL
    };
    int image_status = container_command(image_argv, true);
    if (image_status != 0) {
        if (image_status == 1)
            snprintf(error, error_len,
                     "Podman image 'cma' does not exist; provide a compatible "
                     "image with that tag");
        else
            snprintf(error, error_len,
                     "failed to query Podman image 'cma' (status %d)",
                     image_status);
        goto fail;
    }

    char *exists_argv[] = {
        container->command, "container", "exists", container->name, NULL
    };
    int exists_status = container_command(exists_argv, true);
    if (exists_status == 0) {
        snprintf(error, error_len,
                 "Podman container name is already in use: %s",
                 container->name);
        goto fail;
    }
    if (exists_status != 1) {
        snprintf(error, error_len,
                 "failed to query Podman container %s (status %d)",
                 container->name, exists_status);
        goto fail;
    }

    char *workspace = NULL;
    if (options && options->workspace && options->workspace[0]) {
        workspace = realpath(options->workspace, NULL);
        if (!workspace) {
            snprintf(error, error_len, "workspace %s: %s",
                     options->workspace, strerror(errno));
            goto fail;
        }
    } else {
        workspace = getcwd(NULL, 0);
        if (!workspace) {
            snprintf(error, error_len, "getcwd: %s", strerror(errno));
            goto fail;
        }
    }
    struct stat workspace_stat;
    if (stat(workspace, &workspace_stat) != 0 ||
        !S_ISDIR(workspace_stat.st_mode)) {
        snprintf(error, error_len, "workspace is not a directory: %s",
                 workspace);
        free(workspace);
        goto fail;
    }
    size_t mount_length = strlen(workspace) + 64;
    char *mount = container_xmalloc(mount_length);
    snprintf(mount, mount_length,
             "type=bind,src=%s,dst=/workspace,rw,relabel=shared",
             workspace);
    free(workspace);

    char *run_argv[] = {
        container->command, "run", "--rm", "-d",
        "--name", container->name,
        "--userns=keep-id",
        "--http-proxy=false",
        "--env", "HOME=/tmp/cma-home",
        "--mount", mount,
        "--workdir", "/workspace",
        "cma", NULL
    };
    int run_status = container_command(run_argv, true);
    free(mount);
    if (run_status != 0) {
        snprintf(error, error_len,
                 "failed to start Podman container %s (status %d)",
                 container->name, run_status);
        goto fail;
    }
    container->started = true;

    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
        snprintf(error, error_len, "runner pipe: %s", strerror(errno));
        if (input_pipe[0] >= 0) close(input_pipe[0]);
        if (input_pipe[1] >= 0) close(input_pipe[1]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        if (output_pipe[1] >= 0) close(output_pipe[1]);
        goto fail;
    }
    char context_size[32];
    snprintf(context_size, sizeof(context_size), "%d",
             options && options->context_size > 0 ?
             options->context_size : 100000);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(error, error_len, "runner fork: %s", strerror(errno));
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        goto fail;
    }
    if (pid == 0) {
        close(input_pipe[1]);
        close(output_pipe[0]);
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(input_pipe[0]);
        close(output_pipe[1]);
        if (options && options->edit_upto) {
            execlp(container->command, container->command,
                   "exec", "-i", "--workdir", "/workspace",
                   container->name,
                   "/usr/local/bin/sirio-tool-runner",
                   "--ctx", context_size, "--edit-upto", (char *)NULL);
        } else {
            execlp(container->command, container->command,
                   "exec", "-i", "--workdir", "/workspace",
                   container->name,
                   "/usr/local/bin/sirio-tool-runner",
                   "--ctx", context_size, (char *)NULL);
        }
        fprintf(stderr, "sirio: cannot start tool runner: %s\n",
                strerror(errno));
        _exit(127);
    }
    close(input_pipe[0]);
    close(output_pipe[1]);
    container->input_fd = input_pipe[1];
    container->output_fd = output_pipe[0];
    container->exec_pid = pid;

    char *ready = NULL;
    if (container_read_line(container, &ready, error, error_len) != 0 ||
        strcmp(ready ? ready : "", "READY")) {
        snprintf(error, error_len,
                 "compatible tool runner at "
                 "/usr/local/bin/sirio-tool-runner did not produce READY");
        free(ready);
        goto fail;
    }
    free(ready);
    container->ready = true;
    return container;

fail:
    sirio_container_stop(container);
    return NULL;
}
