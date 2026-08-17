/*
 * sirio_tool_runner.c - self-contained CMA tool runner.
 *
 * This is deliberately one translation unit: it is copied and compiled
 * independently of the Sirio host, then installed in the CMA image.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char *name;
    char *value;
    bool is_string;
} sirio_tool_argument;

typedef struct {
    char *tool;
    sirio_tool_argument *arguments;
    size_t argument_count;
} sirio_tool_request;

int sirio_tool_protocol_decode_request(const char *json,
                                       sirio_tool_request *request,
                                       char *error, size_t error_len);
void sirio_tool_request_free(sirio_tool_request *request);
char *sirio_tool_protocol_encode_response(const char *text, bool is_error);


typedef struct sirio_tool_web sirio_tool_web;

typedef int (*sirio_tool_web_confirm_fn)(void *private_data,
                                         const char *message,
                                         char *error, size_t error_len);
typedef void (*sirio_tool_web_log_fn)(void *private_data,
                                      const char *message);
typedef bool (*sirio_tool_web_cancel_fn)(void *private_data);

typedef struct {
    const char *home_dir;
    int port;
    sirio_tool_web_confirm_fn confirm;
    void *confirm_private_data;
    sirio_tool_web_log_fn log;
    void *log_private_data;
    sirio_tool_web_cancel_fn cancel;
    void *cancel_private_data;
} sirio_tool_web_options;

sirio_tool_web *sirio_tool_web_create(const sirio_tool_web_options *options);
void sirio_tool_web_free(sirio_tool_web *web);
char *sirio_tool_web_google_search(sirio_tool_web *web, const char *query,
                                   char *error, size_t error_len);
char *sirio_tool_web_visit_page(sirio_tool_web *web, const char *url,
                                char *error, size_t error_len);


typedef struct sirio_tool_runtime sirio_tool_runtime;

typedef int (*sirio_tool_confirm_fn)(void *private_data,
                                     const char *message,
                                     char *error, size_t error_len);
typedef void (*sirio_tool_log_fn)(void *private_data, const char *message);

typedef struct {
    const char *home_dir;
    int context_size;
    bool edit_upto;
    sirio_tool_confirm_fn confirm;
    void *confirm_private_data;
    sirio_tool_log_fn log;
    void *log_private_data;
} sirio_tool_options;

sirio_tool_runtime *sirio_tool_runtime_create(
    const sirio_tool_options *options, char *error, size_t error_len);
char *sirio_tool_runtime_execute(
    sirio_tool_runtime *runtime, const char *tool,
    const sirio_tool_argument *arguments, size_t argument_count);
void sirio_tool_runtime_destroy(sirio_tool_runtime *runtime);

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

static int protocol_parse_raw(protocol_parser *parser, char **out) {
    protocol_skip_space(parser);
    const char *start = parser->cur;
    if ((size_t)(parser->end - parser->cur) >= 4 &&
        !memcmp(parser->cur, "true", 4)) {
        parser->cur += 4;
    } else if ((size_t)(parser->end - parser->cur) >= 5 &&
               !memcmp(parser->cur, "false", 5)) {
        parser->cur += 5;
    } else {
        if (parser->cur < parser->end && *parser->cur == '-') parser->cur++;
        const char *integer = parser->cur;
        if (parser->cur < parser->end && *parser->cur == '0') {
            parser->cur++;
        } else {
            while (parser->cur < parser->end &&
                   isdigit((unsigned char)*parser->cur))
                parser->cur++;
            if (parser->cur == integer)
                return protocol_fail(parser, "invalid argument value");
        }
        if (parser->cur < parser->end && *parser->cur == '.') {
            parser->cur++;
            const char *fraction = parser->cur;
            while (parser->cur < parser->end &&
                   isdigit((unsigned char)*parser->cur))
                parser->cur++;
            if (parser->cur == fraction)
                return protocol_fail(parser, "invalid number fraction");
        }
        if (parser->cur < parser->end &&
            (*parser->cur == 'e' || *parser->cur == 'E')) {
            parser->cur++;
            if (parser->cur < parser->end &&
                (*parser->cur == '+' || *parser->cur == '-'))
                parser->cur++;
            const char *exponent = parser->cur;
            while (parser->cur < parser->end &&
                   isdigit((unsigned char)*parser->cur))
                parser->cur++;
            if (parser->cur == exponent)
                return protocol_fail(parser, "invalid number exponent");
        }
    }
    size_t len = (size_t)(parser->cur - start);
    char *value = malloc(len + 1);
    if (!value) return protocol_fail(parser, "out of memory");
    memcpy(value, start, len);
    value[len] = '\0';
    *out = value;
    return 0;
}

static int protocol_parse_arguments(protocol_parser *parser,
                                    sirio_tool_request *request) {
    if (protocol_expect(parser, '{') != 0) return -1;
    protocol_skip_space(parser);
    if (parser->cur < parser->end && *parser->cur == '}') {
        parser->cur++;
        return 0;
    }
    size_t cap = 0;
    for (;;) {
        if (request->argument_count == 128)
            return protocol_fail(parser, "too many tool arguments");
        char *name = NULL;
        char *value = NULL;
        bool is_string = false;
        if (protocol_parse_string(parser, &name) != 0 ||
            protocol_expect(parser, ':') != 0) {
            free(name);
            return -1;
        }
        for (size_t i = 0; i < request->argument_count; i++) {
            if (!strcmp(request->arguments[i].name, name)) {
                free(name);
                return protocol_fail(parser, "duplicate tool argument");
            }
        }
        protocol_skip_space(parser);
        int rc;
        if (parser->cur < parser->end && *parser->cur == '"') {
            is_string = true;
            rc = protocol_parse_string(parser, &value);
        } else {
            rc = protocol_parse_raw(parser, &value);
        }
        if (rc != 0) {
            free(name);
            free(value);
            return -1;
        }
        if (request->argument_count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            sirio_tool_argument *replacement = realloc(
                request->arguments, next_cap * sizeof(*replacement));
            if (!replacement) {
                free(name);
                free(value);
                return protocol_fail(parser, "out of memory");
            }
            request->arguments = replacement;
            cap = next_cap;
        }
        request->arguments[request->argument_count++] =
            (sirio_tool_argument){
                .name = name,
                .value = value,
                .is_string = is_string,
            };
        protocol_skip_space(parser);
        if (parser->cur < parser->end && *parser->cur == '}') {
            parser->cur++;
            return 0;
        }
        if (protocol_expect(parser, ',') != 0) return -1;
    }
}

static void protocol_copy_error(char *error, size_t error_len,
                                const char *message) {
    if (error && error_len)
        snprintf(error, error_len, "%s", message && message[0] ? message :
                 "invalid tool protocol message");
}

int sirio_tool_protocol_decode_request(const char *json,
                                   sirio_tool_request *request,
                                   char *error, size_t error_len) {
    if (!json || !request) {
        protocol_copy_error(error, error_len, "missing request");
        return -1;
    }
    memset(request, 0, sizeof(*request));
    protocol_parser parser = {.cur = json, .end = json + strlen(json)};
    char *key = NULL;
    if (protocol_expect(&parser, '{') != 0 ||
        protocol_parse_string(&parser, &key) != 0 ||
        strcmp(key, "tool") || protocol_expect(&parser, ':') != 0 ||
        protocol_parse_string(&parser, &request->tool) != 0 ||
        protocol_expect(&parser, ',') != 0) {
        free(key);
        goto fail;
    }
    free(key);
    key = NULL;
    if (protocol_parse_string(&parser, &key) != 0 ||
        strcmp(key, "args") || protocol_expect(&parser, ':') != 0 ||
        protocol_parse_arguments(&parser, request) != 0 ||
        protocol_expect(&parser, '}') != 0) {
        free(key);
        goto fail;
    }
    free(key);
    protocol_skip_space(&parser);
    if (parser.cur != parser.end || !request->tool[0]) {
        protocol_fail(&parser, parser.cur != parser.end ?
                      "trailing request data" : "empty tool name");
        goto fail;
    }
    return 0;

fail:
    protocol_copy_error(error, error_len, parser.error);
    sirio_tool_request_free(request);
    return -1;
}

void sirio_tool_request_free(sirio_tool_request *request) {
    if (!request) return;
    free(request->tool);
    for (size_t i = 0; i < request->argument_count; i++) {
        free(request->arguments[i].name);
        free(request->arguments[i].value);
    }
    free(request->arguments);
    memset(request, 0, sizeof(*request));
}

char *sirio_tool_protocol_encode_response(const char *text, bool is_error) {
    protocol_buffer buffer = {0};
    if (protocol_buffer_puts(&buffer, is_error ?
                             "{\"error\":" : "{\"result\":") != 0 ||
        protocol_json_string(&buffer, text ? text : "") != 0 ||
        protocol_buffer_putc(&buffer, '}') != 0) {
        free(buffer.data);
        return NULL;
    }
    return protocol_buffer_take(&buffer);
}
#define WORKER_WEB_DEFAULT_PORT 9333
#define WORKER_WEB_CONNECT_TIMEOUT_MS 3000
#define WORKER_WEB_CDP_TIMEOUT_MS 20000
#define WORKER_WEB_MAX_RESULT_BYTES (1024*1024)

typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
} web_buf;

struct sirio_tool_web {
    char home[PATH_MAX];
    char profile_dir[PATH_MAX];
    int port;
    pid_t chrome_pid;
    bool browser_allowed;
    sirio_tool_web_confirm_fn confirm;
    void *confirm_private_data;
    sirio_tool_web_log_fn log;
    void *log_private_data;
    sirio_tool_web_cancel_fn cancel;
    void *cancel_private_data;
    int next_cdp_id;
};

typedef struct {
    int fd;
    int next_id;
    sirio_tool_web *web;
} cdp_ws;

typedef struct {
    char *id;
    char *ws_url;
} web_tab;

static void *web_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        perror("sirio_tool_web: malloc");
        exit(1);
    }
    return p;
}

static char *web_xstrdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *p = web_xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static void web_buf_append(web_buf *b, const char *s, size_t n) {
    if (!n) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->ptr, cap);
        if (!p) {
            perror("sirio_tool_web: realloc");
            exit(1);
        }
        b->ptr = p;
        b->cap = cap;
    }
    memcpy(b->ptr + b->len, s, n);
    b->len += n;
    b->ptr[b->len] = '\0';
}

static void web_buf_puts(web_buf *b, const char *s) {
    web_buf_append(b, s, strlen(s));
}

static char *web_buf_take(web_buf *b) {
    if (!b->ptr) return web_xstrdup("");
    char *p = b->ptr;
    b->ptr = NULL;
    b->len = b->cap = 0;
    return p;
}

static void web_set_err(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static void web_log(sirio_tool_web *web, const char *msg) {
    if (web && web->log) web->log(web->log_private_data, msg);
}

static double web_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static bool web_cancelled(sirio_tool_web *web) {
    return web && web->cancel && web->cancel(web->cancel_private_data);
}

static bool web_set_cancel_err(sirio_tool_web *web, char *err, size_t err_len) {
    if (!web_cancelled(web)) return false;
    web_set_err(err, err_len, "interrupted");
    return true;
}

static bool web_sleep_ms(sirio_tool_web *web, int ms) {
    int left = ms;
    while (left > 0) {
        if (web_cancelled(web)) return false;
        int step = left < 50 ? left : 50;
        usleep((useconds_t)step * 1000u);
        left -= step;
    }
    return !web_cancelled(web);
}

static bool web_err_is_interrupted(const char *err) {
    return err && !strcmp(err, "interrupted");
}

static bool web_mkdir_p(const char *path) {
    if (!path || !path[0]) return false;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static int web_tcp_connect(const char *host, int port, int timeout_ms,
                           char *err, size_t err_len) {
    char service[32];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, service, &hints, &res);
    if (gai != 0) {
        web_set_err(err, err_len, "getaddrinfo %s: %s", host, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int no_sigpipe = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                         &no_sigpipe, sizeof(no_sigpipe));
#endif
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            if (flags >= 0) fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            rc = poll(&pfd, 1, timeout_ms);
            if (rc > 0) {
                int soerr = 0;
                socklen_t slen = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
                if (soerr == 0) {
                    if (flags >= 0) fcntl(fd, F_SETFL, flags);
                    break;
                }
                errno = soerr;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) web_set_err(err, err_len, "connect %s:%d failed: %s",
                            host, port, strerror(errno));
    return fd;
}

static int web_write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
#else
        ssize_t n = write(fd, p, len);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static ssize_t web_read_some(int fd, char *buf, size_t len, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return rc == 0 ? 0 : -1;
    for (;;) {
        ssize_t n = read(fd, buf, len);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

static bool web_http_expected_size(const web_buf *response,
                                   size_t *expected_size) {
    if (!response || !response->ptr || !expected_size) return false;
    const char *headers_end = strstr(response->ptr, "\r\n\r\n");
    if (!headers_end) return false;

    const char *line = strstr(response->ptr, "\r\n");
    if (!line || line >= headers_end) return false;
    line += 2;
    while (line < headers_end) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > headers_end) line_end = headers_end;
        static const char field[] = "Content-Length:";
        size_t line_len = (size_t)(line_end - line);
        if (line_len >= sizeof(field) - 1 &&
            !strncasecmp(line, field, sizeof(field) - 1)) {
            const char *cursor = line + sizeof(field) - 1;
            while (cursor < line_end &&
                   (*cursor == ' ' || *cursor == '\t'))
                cursor++;
            if (cursor == line_end) return false;
            size_t content_length = 0;
            for (; cursor < line_end; cursor++) {
                if (*cursor < '0' || *cursor > '9') return false;
                unsigned digit = (unsigned)(*cursor - '0');
                if (content_length > (SIZE_MAX - digit) / 10)
                    return false;
                content_length = content_length * 10 + digit;
            }
            size_t header_size = (size_t)(headers_end + 4 - response->ptr);
            if (content_length > SIZE_MAX - header_size) return false;
            *expected_size = header_size + content_length;
            return true;
        }
        line = line_end + 2;
    }
    return false;
}

static char *web_http_request(const char *method, int port, const char *path,
                              char *err, size_t err_len) {
    int fd = web_tcp_connect("127.0.0.1", port, WORKER_WEB_CONNECT_TIMEOUT_MS,
                             err, err_len);
    if (fd < 0) return NULL;
    web_buf req = {0};
    char line[512];
    snprintf(line, sizeof(line),
             "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
             method, path, port);
    web_buf_puts(&req, line);
    if (web_write_all(fd, req.ptr, req.len) != 0) {
        web_set_err(err, err_len, "write HTTP request failed: %s", strerror(errno));
        close(fd);
        free(req.ptr);
        return NULL;
    }
    free(req.ptr);

    web_buf resp = {0};
    char tmp[4096];
    for (;;) {
        ssize_t n = web_read_some(fd, tmp, sizeof(tmp), WORKER_WEB_CONNECT_TIMEOUT_MS);
        if (n < 0) {
            web_set_err(err, err_len, "read HTTP response failed: %s", strerror(errno));
            close(fd);
            free(resp.ptr);
            return NULL;
        }
        if (n == 0) break;
        web_buf_append(&resp, tmp, (size_t)n);
        size_t expected_size = 0;
        if (web_http_expected_size(&resp, &expected_size) &&
            resp.len >= expected_size)
            break;
    }
    close(fd);
    if (!resp.ptr) {
        web_set_err(err, err_len, "empty HTTP response");
        return NULL;
    }
    char *body = strstr(resp.ptr, "\r\n\r\n");
    if (!body) {
        web_set_err(err, err_len, "malformed HTTP response");
        free(resp.ptr);
        return NULL;
    }
    body += 4;
    char *out = web_xstrdup(body);
    free(resp.ptr);
    return out;
}

static bool web_cdp_alive(sirio_tool_web *web) {
    char err[160] = {0};
    char *body = web_http_request("GET", web->port, "/json/version", err, sizeof(err));
    if (!body) return false;
    bool ok = strstr(body, "webSocketDebuggerUrl") != NULL;
    free(body);
    return ok;
}

static char *web_json_get_string(const char *json, const char *key);

static char *web_url_encode(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    web_buf b = {0};
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            web_buf_append(&b, (const char *)&c, 1);
        } else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            web_buf_append(&b, e, 3);
        }
    }
    return web_buf_take(&b);
}

static void web_random_bytes(unsigned char *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t n = read(fd, buf + off, len - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(fd);
        if (off == len) return;
    }
    uint64_t x = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < len; i++) {
        x = x * 6364136223846793005ULL + 1;
        buf[i] = (unsigned char)(x >> 32);
    }
}

static char *web_base64(const unsigned char *data, size_t len) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t outlen = ((len + 2) / 3) * 4;
    char *out = web_xmalloc(outlen + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out[j++] = tab[(v >> 18) & 63];
        out[j++] = tab[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? tab[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? tab[v & 63] : '=';
    }
    out[j] = '\0';
    return out;
}

static char *web_json_quote(const char *s) {
    web_buf b = {0};
    web_buf_puts(&b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '\\': web_buf_puts(&b, "\\\\"); break;
        case '"': web_buf_puts(&b, "\\\""); break;
        case '\n': web_buf_puts(&b, "\\n"); break;
        case '\r': web_buf_puts(&b, "\\r"); break;
        case '\t': web_buf_puts(&b, "\\t"); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                web_buf_puts(&b, tmp);
            } else {
                web_buf_append(&b, (const char *)&c, 1);
            }
            break;
        }
    }
    web_buf_puts(&b, "\"");
    return web_buf_take(&b);
}

static int web_ws_connect(const char *ws_url, cdp_ws *ws,
                          char *err, size_t err_len) {
    const char *p = ws_url;
    if (strncmp(p, "ws://", 5) != 0) {
        web_set_err(err, err_len, "unsupported websocket URL: %s", ws_url);
        return -1;
    }
    p += 5;
    const char *slash = strchr(p, '/');
    if (!slash) {
        web_set_err(err, err_len, "malformed websocket URL");
        return -1;
    }
    char hostport[256];
    size_t hp_len = (size_t)(slash - p);
    if (hp_len >= sizeof(hostport)) hp_len = sizeof(hostport) - 1;
    memcpy(hostport, p, hp_len);
    hostport[hp_len] = '\0';
    char *colon = strrchr(hostport, ':');
    int port = 80;
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    const char *host = hostport;
    int fd = web_tcp_connect(host, port, WORKER_WEB_CONNECT_TIMEOUT_MS, err, err_len);
    if (fd < 0) return -1;

    unsigned char rnd[16];
    web_random_bytes(rnd, sizeof(rnd));
    char *key = web_base64(rnd, sizeof(rnd));
    web_buf req = {0};
    char line[512];
    snprintf(line, sizeof(line),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             slash, host, port, key);
    web_buf_puts(&req, line);
    free(key);
    if (web_write_all(fd, req.ptr, req.len) != 0) {
        web_set_err(err, err_len, "websocket handshake write failed");
        close(fd);
        free(req.ptr);
        return -1;
    }
    free(req.ptr);

    web_buf resp = {0};
    char tmp[1024];
    double deadline = web_now_sec() + (double)WORKER_WEB_CONNECT_TIMEOUT_MS / 1000.0;
    while (!strstr(resp.ptr ? resp.ptr : "", "\r\n\r\n")) {
        if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) {
            close(fd);
            free(resp.ptr);
            return -1;
        }
        double now = web_now_sec();
        if (now >= deadline) {
            web_set_err(err, err_len, "websocket handshake read failed");
            close(fd);
            free(resp.ptr);
            return -1;
        }
        int slice = 100;
        int remaining = (int)((deadline - now) * 1000.0);
        if (remaining < slice) slice = remaining > 0 ? remaining : 1;
        ssize_t n = web_read_some(fd, tmp, sizeof(tmp), slice);
        if (n < 0) {
            web_set_err(err, err_len, "websocket handshake read failed");
            close(fd);
            free(resp.ptr);
            return -1;
        }
        if (n == 0) continue;
        web_buf_append(&resp, tmp, (size_t)n);
        if (resp.len > 8192) break;
    }
    bool ok = resp.ptr && strstr(resp.ptr, " 101 ") != NULL;
    free(resp.ptr);
    if (!ok) {
        web_set_err(err, err_len, "websocket handshake rejected");
        close(fd);
        return -1;
    }
    ws->fd = fd;
    ws->next_id = 1;
    return 0;
}

static void web_ws_close(cdp_ws *ws) {
    if (ws && ws->fd >= 0) {
        close(ws->fd);
        ws->fd = -1;
    }
}

static int web_read_exact(cdp_ws *ws, unsigned char *buf, size_t len,
                          int timeout_ms, char *err, size_t err_len) {
    size_t off = 0;
    double deadline = web_now_sec() + (double)timeout_ms / 1000.0;
    while (off < len) {
        if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) return -1;
        double now = web_now_sec();
        if (now >= deadline) {
            web_set_err(err, err_len, "websocket read timeout");
            return -1;
        }
        int slice = 100;
        int remaining = (int)((deadline - now) * 1000.0);
        if (remaining < slice) slice = remaining > 0 ? remaining : 1;
        ssize_t n = web_read_some(ws->fd, (char *)buf + off, len - off, slice);
        if (n < 0) {
            web_set_err(err, err_len, "websocket frame read failed");
            return -1;
        }
        if (n == 0) continue;
        off += (size_t)n;
    }
    return 0;
}

static int web_ws_send_text(cdp_ws *ws, const char *text,
                            char *err, size_t err_len) {
    size_t len = strlen(text);
    web_buf frame = {0};
    unsigned char hdr[14];
    size_t h = 0;
    hdr[h++] = 0x81;
    if (len < 126) {
        hdr[h++] = 0x80 | (unsigned char)len;
    } else if (len <= 0xffff) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)len;
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((uint64_t)len >> (i * 8));
    }
    unsigned char mask[4];
    web_random_bytes(mask, sizeof(mask));
    for (int i = 0; i < 4; i++) hdr[h++] = mask[i];
    web_buf_append(&frame, (const char *)hdr, h);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = ((const unsigned char *)text)[i] ^ mask[i & 3];
        web_buf_append(&frame, (const char *)&c, 1);
    }
    int rc = web_write_all(ws->fd, frame.ptr, frame.len);
    free(frame.ptr);
    if (rc != 0) {
        web_set_err(err, err_len, "websocket write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int web_ws_send_pong(cdp_ws *ws, const unsigned char *payload, size_t len) {
    if (len > 125) len = 125;
    unsigned char hdr[2 + 4 + 125];
    hdr[0] = 0x8a;
    hdr[1] = 0x80 | (unsigned char)len;
    unsigned char mask[4];
    web_random_bytes(mask, sizeof(mask));
    memcpy(hdr + 2, mask, 4);
    for (size_t i = 0; i < len; i++) hdr[6 + i] = payload[i] ^ mask[i & 3];
    return web_write_all(ws->fd, hdr, 6 + len);
}

static char *web_ws_read_message(cdp_ws *ws, char *err, size_t err_len) {
    web_buf msg = {0};
    for (;;) {
        unsigned char h[2];
        if (web_read_exact(ws, h, 2, WORKER_WEB_CDP_TIMEOUT_MS, err, err_len) != 0) {
            free(msg.ptr);
            return NULL;
        }
        bool fin = (h[0] & 0x80) != 0;
        int opcode = h[0] & 0x0f;
        bool masked = (h[1] & 0x80) != 0;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            unsigned char x[2];
            if (web_read_exact(ws, x, 2, WORKER_WEB_CDP_TIMEOUT_MS, err, err_len) != 0) goto fail;
            len = ((uint64_t)x[0] << 8) | x[1];
        } else if (len == 127) {
            unsigned char x[8];
            if (web_read_exact(ws, x, 8, WORKER_WEB_CDP_TIMEOUT_MS, err, err_len) != 0) goto fail;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | x[i];
        }
        unsigned char mask[4] = {0};
        if (masked && web_read_exact(ws, mask, 4, WORKER_WEB_CDP_TIMEOUT_MS, err, err_len) != 0)
            goto fail;
        if (len > WORKER_WEB_MAX_RESULT_BYTES * 4ULL) {
            web_set_err(err, err_len, "websocket message too large");
            free(msg.ptr);
            return NULL;
        }
        unsigned char *payload = web_xmalloc((size_t)len + 1);
        if (len && web_read_exact(ws, payload, (size_t)len,
                                  WORKER_WEB_CDP_TIMEOUT_MS, err, err_len) != 0) {
            free(payload);
            goto fail;
        }
        for (uint64_t i = 0; masked && i < len; i++) payload[i] ^= mask[i & 3];
        payload[len] = '\0';
        if (opcode == 0x8) {
            free(payload);
            web_set_err(err, err_len, "websocket closed");
            free(msg.ptr);
            return NULL;
        } else if (opcode == 0x9) {
            web_ws_send_pong(ws, payload, (size_t)len);
            free(payload);
            continue;
        } else if (opcode == 0x1 || opcode == 0x0) {
            web_buf_append(&msg, (const char *)payload, (size_t)len);
            free(payload);
            if (fin) return web_buf_take(&msg);
        } else {
            free(payload);
        }
    }
fail:
    if (err && err_len && !err[0]) web_set_err(err, err_len, "websocket frame read failed");
    free(msg.ptr);
    return NULL;
}

static bool web_json_id_matches(const char *json, int id) {
    const char *p = strstr(json, "\"id\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p) == id;
}

static char *web_cdp_call(cdp_ws *ws, const char *method, const char *params,
                          char *err, size_t err_len) {
    if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) return NULL;
    int id = ws->next_id++;
    web_buf req = {0};
    char head[256];
    snprintf(head, sizeof(head), "{\"id\":%d,\"method\":", id);
    web_buf_puts(&req, head);
    char *qmethod = web_json_quote(method);
    web_buf_puts(&req, qmethod);
    free(qmethod);
    if (params && params[0]) {
        web_buf_puts(&req, ",\"params\":");
        web_buf_puts(&req, params);
    }
    web_buf_puts(&req, "}");
    char *wire = web_buf_take(&req);
    if (web_ws_send_text(ws, wire, err, err_len) != 0) {
        free(wire);
        return NULL;
    }
    free(wire);
    for (;;) {
        if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) return NULL;
        char *msg = web_ws_read_message(ws, err, err_len);
        if (!msg) return NULL;
        if (web_json_id_matches(msg, id)) return msg;
        free(msg);
    }
}

static void web_cdp_call_optional(cdp_ws *ws, const char *method, const char *params) {
    char err[160] = {0};
    char *resp = web_cdp_call(ws, method, params, err, sizeof(err));
    free(resp);
}

static int web_hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int x;
        if (c >= '0' && c <= '9') x = c - '0';
        else if (c >= 'a' && c <= 'f') x = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') x = c - 'A' + 10;
        else return -1;
        v = (v << 4) | x;
    }
    return v;
}

static void web_utf8_append(web_buf *b, unsigned code) {
    char out[4];
    if (code <= 0x7f) {
        out[0] = (char)code;
        web_buf_append(b, out, 1);
    } else if (code <= 0x7ff) {
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        web_buf_append(b, out, 2);
    } else if (code <= 0xffff) {
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        web_buf_append(b, out, 3);
    } else {
        out[0] = (char)(0xf0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[3] = (char)(0x80 | (code & 0x3f));
        web_buf_append(b, out, 4);
    }
}

static char *web_json_parse_string_at(const char *q, const char **endp) {
    if (*q != '"') return NULL;
    q++;
    web_buf b = {0};
    while (*q && *q != '"') {
        if (*q != '\\') {
            web_buf_append(&b, q++, 1);
            continue;
        }
        q++;
        switch (*q) {
        case '"': web_buf_append(&b, "\"", 1); q++; break;
        case '\\': web_buf_append(&b, "\\", 1); q++; break;
        case '/': web_buf_append(&b, "/", 1); q++; break;
        case 'b': web_buf_append(&b, "\b", 1); q++; break;
        case 'f': web_buf_append(&b, "\f", 1); q++; break;
        case 'n': web_buf_append(&b, "\n", 1); q++; break;
        case 'r': web_buf_append(&b, "\r", 1); q++; break;
        case 't': web_buf_append(&b, "\t", 1); q++; break;
        case 'u': {
            int v = web_hex4(q + 1);
            if (v < 0) { free(b.ptr); return NULL; }
            q += 5;
            if (v >= 0xd800 && v <= 0xdbff && q[0] == '\\' && q[1] == 'u') {
                int lo = web_hex4(q + 2);
                if (lo >= 0xdc00 && lo <= 0xdfff) {
                    unsigned code = 0x10000 + (((unsigned)v - 0xd800) << 10) +
                                    ((unsigned)lo - 0xdc00);
                    web_utf8_append(&b, code);
                    q += 6;
                    break;
                }
            }
            web_utf8_append(&b, (unsigned)v);
            break;
        }
        default:
            if (*q) web_buf_append(&b, q++, 1);
            break;
        }
    }
    if (*q != '"') {
        free(b.ptr);
        return NULL;
    }
    if (endp) *endp = q + 1;
    return web_buf_take(&b);
}

static char *web_json_get_string(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p++ != ':') continue;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '"') return web_json_parse_string_at(p, NULL);
    }
    return NULL;
}

static char *web_cdp_eval_string(cdp_ws *ws, const char *expr,
                                 char *err, size_t err_len) {
    char *qexpr = web_json_quote(expr);
    web_buf params = {0};
    web_buf_puts(&params, "{\"expression\":");
    web_buf_puts(&params, qexpr);
    web_buf_puts(&params, ",\"returnByValue\":true,\"awaitPromise\":true,\"includeCommandLineAPI\":true}");
    free(qexpr);
    char *params_s = web_buf_take(&params);
    char *resp = web_cdp_call(ws, "Runtime.evaluate", params_s, err, err_len);
    free(params_s);
    if (!resp) return NULL;
    if (strstr(resp, "\"exceptionDetails\"")) {
        web_set_err(err, err_len, "JavaScript evaluation failed");
        free(resp);
        return NULL;
    }
    char *val = web_json_get_string(resp, "value");
    free(resp);
    if (!val) web_set_err(err, err_len, "Runtime.evaluate did not return a string");
    return val;
}

static bool web_wait_ready(cdp_ws *ws, char *err, size_t err_len) {
    const char *expr = "document.readyState";
    for (int i = 0; i < 80; i++) {
        if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) return false;
        char *state = web_cdp_eval_string(ws, expr, err, err_len);
        if (state && (!strcmp(state, "complete") || !strcmp(state, "interactive"))) {
            free(state);
            if (web_sleep_ms(ws ? ws->web : NULL, 100)) return true;
            web_set_err(err, err_len, "interrupted");
            return false;
        }
        free(state);
        if (!web_sleep_ms(ws ? ws->web : NULL, 250)) {
            web_set_err(err, err_len, "interrupted");
            return false;
        }
    }
    web_set_err(err, err_len, "page did not become ready before timeout");
    return false;
}

static bool web_cdp_navigate(cdp_ws *ws, const char *url,
                             char *err, size_t err_len) {
    char *qurl = web_json_quote(url);
    web_buf params = {0};
    web_buf_puts(&params, "{\"url\":");
    web_buf_puts(&params, qurl);
    web_buf_puts(&params, "}");
    free(qurl);
    char *params_s = web_buf_take(&params);
    char *resp = web_cdp_call(ws, "Page.navigate", params_s, err, err_len);
    free(params_s);
    if (!resp) return false;
    free(resp);
    return true;
}

static bool web_page_probe(cdp_ws *ws, char **href_out, char **ready_out,
                           long *text_len_out, char *err, size_t err_len) {
    const char *expr =
        "location.href+'\\n'+document.readyState+'\\n'+"
        "((document.body&&document.body.textContent)||'').length";
    char *probe = web_cdp_eval_string(ws, expr, err, err_len);
    if (!probe) return false;

    char *nl1 = strchr(probe, '\n');
    char *nl2 = nl1 ? strchr(nl1 + 1, '\n') : NULL;
    if (!nl1 || !nl2) {
        free(probe);
        web_set_err(err, err_len, "page readiness probe returned malformed data");
        return false;
    }
    *nl1 = '\0';
    *nl2 = '\0';
    if (href_out) *href_out = web_xstrdup(probe);
    if (ready_out) *ready_out = web_xstrdup(nl1 + 1);
    if (text_len_out) *text_len_out = strtol(nl2 + 1, NULL, 10);
    free(probe);
    return true;
}

static bool web_wait_navigated_ready(cdp_ws *ws, const char *url,
                                     char *err, size_t err_len) {
    long last_len = -1;
    char *last_href = NULL;
    int stable = 0;

    for (int i = 0; i < 100; i++) {
        if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) {
            free(last_href);
            return false;
        }
        char *href = NULL;
        char *ready = NULL;
        long text_len = 0;
        bool ok = web_page_probe(ws, &href, &ready, &text_len, err, err_len);
        if (!ok) {
            free(href);
            free(ready);
            if (!web_sleep_ms(ws ? ws->web : NULL, 250)) {
                free(last_href);
                web_set_err(err, err_len, "interrupted");
                return false;
            }
            continue;
        }

        bool real_url = href && href[0] &&
                        strcmp(href, "about:blank") &&
                        strncmp(href, "chrome://", 9);
        bool ready_state = ready &&
            (!strcmp(ready, "complete") || !strcmp(ready, "interactive"));
        bool same_page = real_url && last_href && !strcmp(href, last_href) &&
                         text_len == last_len;
        stable = ready_state && same_page ? stable + 1 : 0;
        free(last_href);
        last_href = real_url ? web_xstrdup(href) : NULL;
        last_len = text_len;

        free(href);
        free(ready);

        if (real_url && ready_state && stable >= 2) {
            free(last_href);
            if (web_sleep_ms(ws ? ws->web : NULL, 200)) return true;
            web_set_err(err, err_len, "interrupted");
            return false;
        }
        if (!web_sleep_ms(ws ? ws->web : NULL, 250)) {
            free(last_href);
            web_set_err(err, err_len, "interrupted");
            return false;
        }
    }
    free(last_href);
    web_set_err(err, err_len, "navigation to %s did not settle before timeout",
                url ? url : "page");
    return false;
}

static bool web_cdp_prepare_page(cdp_ws *ws, char *err, size_t err_len) {
    char *resp = web_cdp_call(ws, "Page.enable", "{}", err, err_len);
    if (!resp) return false;
    free(resp);
    resp = web_cdp_call(ws, "Runtime.enable", "{}", err, err_len);
    if (!resp) return false;
    free(resp);
    web_cdp_call_optional(ws, "Emulation.setFocusEmulationEnabled",
                          "{\"enabled\":true}");
    web_cdp_call_optional(ws, "Emulation.setDeviceMetricsOverride",
                          "{\"width\":1365,\"height\":900,\"deviceScaleFactor\":1,\"mobile\":false}");
    return web_wait_ready(ws, err, err_len);
}

static bool web_scroll_dynamic_page(cdp_ws *ws, char *err, size_t err_len) {
    const char *expr =
        "(() => new Promise(resolve => {"
        "const root=()=>document.scrollingElement||document.documentElement||document.body;"
        "const blockSel='h1,h2,h3,h4,h5,h6,p,li,pre,blockquote,td,th,[id=\"content-text\"],[class*=\"comment-body\"],[class*=\"comment-content\"],[data-testid*=\"comment-text\"]';"
        "const lazySel='[onscroll],[loading=\"lazy\"],[data-src],[data-lazy],[class*=\"lazy\"],[class*=\"infinite\"],[class*=\"virtual\"],[role=\"feed\"],[id*=\"comment\"],[class*=\"comment\"],[data-testid*=\"comment\"]';"
        "const hookCount=()=>{let n=0;try{if(window.onscroll)n++;if(document.onscroll)n++;if(document.body&&document.body.onscroll)n++;}catch(e){}"
        "try{if(typeof getEventListeners==='function'){for(const o of [window,document,document.body]){if(!o)continue;const ev=getEventListeners(o);if(ev&&ev.scroll)n+=ev.scroll.length;}}}catch(e){}"
        "try{n+=document.querySelectorAll(lazySel).length;}catch(e){}return n;};"
        "const metrics=()=>{const r=root();return {"
        "height:r?r.scrollHeight:0,"
        "view:innerHeight||900,"
        "y:scrollY||(r&&r.scrollTop)||0,"
        "text:((document.body&&document.body.innerText)||'').length,"
        "links:document.links?document.links.length:0,"
        "blocks:document.body?document.body.querySelectorAll(blockSel).length:0,"
        "hooks:hookCount()};};"
        "const sig=m=>[m.height,m.text,m.links,m.blocks].join('|');"
        "const grew=(a,b)=>b.height>a.height+20||b.text>a.text+200||b.links>a.links+2||b.blocks>a.blocks+2;"
        "const scrollOnce=()=>{const r=root();if(!r)return;"
        "const h=Math.max(700,Math.floor((innerHeight||900)*0.85));"
        "window.scrollTo(0,Math.min(r.scrollHeight,(scrollY||r.scrollTop||0)+h));};"
        "let last=metrics(),lastSig=sig(last),same=0,steps=0;"
        "const scrollable=last.height>last.view*1.35;"
        "if(!scrollable||last.hooks===0){resolve('scroll skipped hooks='+last.hooks+' text='+last.text);return;}"
        "const tick=()=>{"
        "if(steps>=28){resolve('scrolled '+steps+' text='+last.text);return;}"
        "const before=last;"
        "scrollOnce();steps++;"
        "setTimeout(()=>{const now=metrics(),nowSig=sig(now);"
        "if(nowSig===lastSig)same++;else same=0;"
        "const loaded=grew(before,now);"
        "last=now;lastSig=nowSig;"
        "if(steps===1&&!loaded){resolve('scroll probe unchanged text='+now.text);return;}"
        "const atBottom=now.y+now.view+20>=now.height;"
        "if(same>=4||(atBottom&&same>=1)){resolve('scrolled '+steps+' text='+now.text);return;}"
        "tick();},900);"
        "};tick();"
        "}))()";
    if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) return false;

    char local_err[160] = {0};
    char *res = web_cdp_eval_string(ws, expr, local_err, sizeof(local_err));
    if (!res && web_err_is_interrupted(local_err)) {
        web_set_err(err, err_len, "interrupted");
        return false;
    }
    free(res);
    if (web_set_cancel_err(ws ? ws->web : NULL, err, err_len)) return false;
    return true;
}

static char *web_chrome_executable(void) {
    const char *env = getenv("SIRIO_CHROME");
    if (env && env[0]) return web_xstrdup(env);
#ifdef __APPLE__
    if (access("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", X_OK) == 0)
        return web_xstrdup("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
    if (access("/Applications/Chromium.app/Contents/MacOS/Chromium", X_OK) == 0)
        return web_xstrdup("/Applications/Chromium.app/Contents/MacOS/Chromium");
#endif
    const char *paths[] = {
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
        "/snap/bin/chromium",
        "/opt/google/chrome/chrome",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) return web_xstrdup(paths[i]);
    }

    const char *names[] = {
        "google-chrome",
        "google-chrome-stable",
        "chromium",
        "chromium-browser",
        NULL
    };
    const char *pathenv = getenv("PATH");
    if (pathenv) {
        char *path = web_xstrdup(pathenv);
        char *save = NULL;
        for (char *dir = strtok_r(path, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
            for (int i = 0; names[i]; i++) {
                char candidate[PATH_MAX];
                snprintf(candidate, sizeof(candidate), "%s/%s", dir[0] ? dir : ".", names[i]);
                if (access(candidate, X_OK) == 0) {
                    char *res = web_xstrdup(candidate);
                    free(path);
                    return res;
                }
            }
        }
        free(path);
    }
    return NULL;
}

static void web_stop_chrome(sirio_tool_web *web) {
    if (!web || web->chrome_pid <= 0) return;
    pid_t pid = web->chrome_pid;
    web->chrome_pid = 0;
    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid || (waited < 0 && errno == ECHILD)) return;
    (void)kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) return;
        usleep(50000);
    }
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static bool web_spawn_chrome(sirio_tool_web *web, char *err, size_t err_len) {
    if (!web_mkdir_p(web->profile_dir)) {
        web_set_err(err, err_len, "failed to create Chrome profile dir %s: %s",
                    web->profile_dir, strerror(errno));
        return false;
    }
    char *exe = web_chrome_executable();
    if (!exe) {
        web_set_err(err, err_len,
                    "Chrome or Chromium is not installed; set SIRIO_CHROME to a supported executable");
        return false;
    }
    char port_arg[64], profile_arg[PATH_MAX + 64];
    snprintf(port_arg, sizeof(port_arg), "--remote-debugging-port=%d", web->port);
    snprintf(profile_arg, sizeof(profile_arg), "--user-data-dir=%s", web->profile_dir);
    pid_t pid = fork();
    if (pid < 0) {
        web_set_err(err, err_len, "failed to fork Chrome: %s", strerror(errno));
        free(exe);
        return false;
    }
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            if (nullfd > 2) close(nullfd);
        }
#ifdef __APPLE__
        execlp(exe, exe, port_arg, "--remote-allow-origins=*",
               profile_arg, "--no-first-run", "--no-default-browser-check",
               "--disable-sync", "--use-mock-keychain", "--password-store=basic",
               "--mute-audio", "--headless=new", "about:blank", (char *)NULL);
#else
        if (geteuid() == 0) {
            execlp(exe, exe, port_arg, "--remote-allow-origins=*",
                   profile_arg, "--no-first-run", "--no-default-browser-check",
                   "--disable-sync", "--password-store=basic", "--no-sandbox",
                   "--mute-audio", "--headless=new", "about:blank", (char *)NULL);
        } else {
            execlp(exe, exe, port_arg, "--remote-allow-origins=*",
                   profile_arg, "--no-first-run", "--no-default-browser-check",
                   "--disable-sync", "--password-store=basic",
                   "--mute-audio", "--headless=new", "about:blank", (char *)NULL);
        }
#endif
        _exit(127);
    }
    free(exe);
    web->chrome_pid = pid;
    for (int i = 0; i < 80; i++) {
        if (web_set_cancel_err(web, err, err_len)) return false;
        if (web_cdp_alive(web)) {
            web_log(web, "Chrome browser session is ready");
            return true;
        }
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            web->chrome_pid = 0;
            web_set_err(err, err_len, "Chrome exited before CDP became ready");
            return false;
        }
        if (!web_sleep_ms(web, 250)) {
            web_set_err(err, err_len, "interrupted");
            return false;
        }
    }
    web_set_err(err, err_len, "Chrome did not expose CDP on port %d", web->port);
    return false;
}

static bool web_ensure_browser(sirio_tool_web *web, char *err, size_t err_len) {
    if (web_cdp_alive(web)) return true;
    if (web->chrome_pid > 0) {
        int status = 0;
        pid_t waited = waitpid(web->chrome_pid, &status, WNOHANG);
        if (waited == web->chrome_pid) web->chrome_pid = 0;
        else web_stop_chrome(web);
    }
    if (!web->browser_allowed) {
        if (!web->confirm) {
            web_set_err(err, err_len,
                        "starting Chrome requires interactive approval");
            return false;
        }
        if (!web->confirm(web->confirm_private_data,
                          "The web tool wants to start Chrome. Allow? (y/n) ",
                          err, err_len))
        {
            if (err && !err[0]) web_set_err(err, err_len, "user denied Chrome browser start");
            return false;
        }
        web->browser_allowed = true;
    }
    return web_spawn_chrome(web, err, err_len);
}

static void web_tab_free(web_tab *tab) {
    if (!tab) return;
    free(tab->id);
    free(tab->ws_url);
    tab->id = NULL;
    tab->ws_url = NULL;
}

static char *web_browser_ws_url(sirio_tool_web *web, char *err, size_t err_len) {
    char *body = web_http_request("GET", web->port, "/json/version", err, err_len);
    if (!body) return NULL;
    char *ws = web_json_get_string(body, "webSocketDebuggerUrl");
    free(body);
    if (!ws) web_set_err(err, err_len, "Chrome did not return a browser WebSocket URL");
    return ws;
}

static bool web_open_tab(sirio_tool_web *web, const char *url, web_tab *tab,
                         char *err, size_t err_len) {
    memset(tab, 0, sizeof(*tab));

    char *browser_url = web_browser_ws_url(web, err, err_len);
    if (!browser_url) return false;
    cdp_ws browser = {.fd = -1, .web = web};
    if (web_ws_connect(browser_url, &browser, err, err_len) != 0) {
        free(browser_url);
        return false;
    }
    free(browser_url);

    char *qurl = web_json_quote(url);
    web_buf params = {0};
    web_buf_puts(&params, "{\"url\":");
    web_buf_puts(&params, qurl);
    web_buf_puts(&params, ",\"background\":true,\"newWindow\":false}");
    free(qurl);
    char *params_s = web_buf_take(&params);
    char *resp = web_cdp_call(&browser, "Target.createTarget",
                              params_s, err, err_len);
    free(params_s);
    web_ws_close(&browser);
    if (!resp) return false;

    tab->id = web_json_get_string(resp, "targetId");
    free(resp);
    if (!tab->id) {
        web_tab_free(tab);
        web_set_err(err, err_len, "Chrome did not return a page target id");
        return false;
    }

    char ws_url[PATH_MAX + 128];
    snprintf(ws_url, sizeof(ws_url), "ws://127.0.0.1:%d/devtools/page/%s",
             web->port, tab->id);
    tab->ws_url = web_xstrdup(ws_url);
    return true;
}

static void web_close_tab(sirio_tool_web *web, const web_tab *tab) {
    if (!web || !tab || !tab->id || !tab->id[0]) return;
    char *enc = web_url_encode(tab->id);
    web_buf path = {0};
    web_buf_puts(&path, "/json/close/");
    web_buf_puts(&path, enc);
    free(enc);

    char err[160] = {0};
    char *path_s = web_buf_take(&path);
    char *body = web_http_request("GET", web->port, path_s, err, sizeof(err));
    free(path_s);
    if (body) {
        free(body);
    } else if (err[0]) {
        web_log(web, err);
    }
}

static const char *web_click_google_consent_js =
"(() => {"
"const host=location.hostname.toLowerCase();"
"if(!/(^|\\.)google\\.[a-z.]+$/.test(host))return '';"
"const clean=s=>(s||'').replace(/\\s+/g,' ').trim();"
"const pats=[/^accept all$/i,/^i agree$/i,/^accetta tutto$/i,/^tout accepter$/i,/^aceptar todo$/i,/^alle akzeptieren$/i];"
"const els=[...document.querySelectorAll('button,[role=button],input[type=submit],a')];"
"for (const el of els){const t=clean(el.innerText||el.value||el.textContent);"
"if(!t)continue; if(pats.some(p=>p.test(t))){el.click(); return 'clicked '+t;}}"
"return '';"
"})()";

static const char *web_extract_search_js =
"(() => {"
"const clean=s=>(s||'').replace(/\\s+/g,' ').trim();"
"const esc=s=>clean(s).replace(/\\\\/g,'\\\\\\\\').replace(/\\[/g,'\\\\[').replace(/\\]/g,'\\\\]').replace(/\\n/g,' ');"
"const visible=el=>{const r=el.getBoundingClientRect();const st=getComputedStyle(el);return r.width>0&&r.height>0&&st.display!=='none'&&st.visibility!=='hidden'&&st.opacity!=='0';};"
"const bad=h=>(/(^|\\.)google\\./.test(h)||/(^|\\.)gstatic\\./.test(h)||/(^|\\.)googleusercontent\\./.test(h));"
"const lines=['# Google search results','',`URL: ${location.href}`,'','## Visible links'];"
"const seen=new Set();"
"for(const a of document.querySelectorAll('a[href]')){if(!visible(a))continue;let href=a.href||'';"
"try{const u=new URL(href);if(u.pathname==='/url'&&u.searchParams.get('q'))href=u.searchParams.get('q');}catch{}"
"let u;try{u=new URL(href);}catch{continue;}if(!/^https?:$/.test(u.protocol))continue;if(bad(u.hostname))continue;"
"const text=esc(a.innerText||a.textContent);if(text.length<3)continue;if(seen.has(u.href))continue;seen.add(u.href);"
"lines.push(`- [${text.slice(0,180)}](${u.href})`);if(seen.size>=20)break;}"
"lines.push('','## Text snapshot',clean(document.body.innerText).slice(0,1200));"
"return lines.join('\\n');"
"})()";

static const char *web_extract_page_js =
"(() => {"
"const clean=s=>(s||'').replace(/\\s+/g,' ').trim();"
"const esc=s=>clean(s).replace(/\\\\/g,'\\\\\\\\').replace(/\\[/g,'\\\\[').replace(/\\]/g,'\\\\]').replace(/\\n/g,' ');"
"const visible=el=>{const r=el.getBoundingClientRect();const st=getComputedStyle(el);return r.width>0&&r.height>0&&st.display!=='none'&&st.visibility!=='hidden'&&st.opacity!=='0';};"
"const inline=n=>{if(!n)return'';if(n.nodeType===3)return n.nodeValue;if(n.nodeType!==1)return'';const el=n;"
"if(el.tagName==='SCRIPT'||el.tagName==='STYLE'||el.tagName==='NOSCRIPT')return'';"
"if(el.tagName==='A'){const t=esc(el.innerText||el.textContent);const h=el.href||'';return t&&h?`[${t}](${h})`:t;}"
"if(el.tagName==='CODE')return '`'+clean(el.innerText||el.textContent).replace(/`/g,'\\\\`')+'`';"
"return [...el.childNodes].map(inline).join('');};"
"const lines=[`# ${clean(document.title)||location.href}`,'',`URL: ${location.href}`,'','## Content'];"
"const blocks=[...document.body.querySelectorAll('h1,h2,h3,h4,h5,h6,p,li,pre,blockquote,td,th,[id=\"content-text\"],[class*=\"comment-body\"],[class*=\"comment-content\"],[data-testid*=\"comment-text\"]')];"
"const seen=new Set();"
"for(const el of blocks){if(!visible(el))continue;let s='';const tag=el.tagName;"
"if(/^H[1-6]$/.test(tag)){s='#'.repeat(Number(tag[1]))+' '+inline(el);}"
"else if(tag==='LI'){s='- '+inline(el);}"
"else if(tag==='PRE'){s='```\\n'+(el.innerText||el.textContent||'').trimEnd()+'\\n```';}"
"else if(tag==='BLOCKQUOTE'){s='> '+clean(el.innerText||el.textContent);}"
"else{s=inline(el);}s=s.trim();if(!s||seen.has(s))continue;seen.add(s);lines.push('',s);"
"if(lines.join('\\n').length>900000){lines.push('','[Content truncated by browser extractor.]');break;}}"
"lines.push('','## Visible links');let n=0;const linkSeen=new Set();"
"for(const a of document.querySelectorAll('a[href]')){if(!visible(a))continue;const t=esc(a.innerText||a.textContent);if(t.length<3)continue;"
"let u;try{u=new URL(a.href);}catch{continue;}if(!/^https?:$/.test(u.protocol)||linkSeen.has(u.href))continue;linkSeen.add(u.href);"
"lines.push(`- [${t.slice(0,160)}](${u.href})`);if(++n>=80)break;}"
"return lines.join('\\n');"
"})()";

static bool web_google_blocked(const char *href, const char *body) {
    if (href && strstr(href, "google.") &&
        (strstr(href, "/sorry/") || strstr(href, "/sorry?")))
        return true;
    return body &&
        (strcasestr(body, "unusual traffic from your computer network") ||
         strcasestr(body, "may be sending automated queries") ||
         strcasestr(body, "our systems have detected unusual traffic"));
}

static int web_check_google_block(cdp_ws *ws, char *err, size_t err_len) {
    const char *expr =
        "location.href+'\\n'+"
        "(((document.body&&document.body.innerText)||'').slice(0,12000))";
    char *snapshot = web_cdp_eval_string(ws, expr, err, err_len);
    if (!snapshot) return -1;
    char *body = strchr(snapshot, '\n');
    if (body) *body++ = '\0';
    bool blocked = web_google_blocked(snapshot, body ? body : "");
    free(snapshot);
    if (!blocked) return 0;
    web_set_err(err, err_len,
                "Google blocked automated search traffic; use visit_page or bash/curl for a known URL");
    return 1;
}

static char *web_run_page_js(sirio_tool_web *web, const char *url, const char *js,
                             bool dynamic_scroll, bool google_search,
                             char *err, size_t err_len) {
    if (!web_ensure_browser(web, err, err_len)) return NULL;
    web_tab tab = {0};
    if (!web_open_tab(web, "about:blank", &tab, err, err_len)) return NULL;
    cdp_ws ws = {.fd = -1, .web = web};
    if (web_ws_connect(tab.ws_url, &ws, err, err_len) != 0) {
        web_close_tab(web, &tab);
        web_tab_free(&tab);
        return NULL;
    }
    if (!web_cdp_prepare_page(&ws, err, err_len)) {
        web_ws_close(&ws);
        web_close_tab(web, &tab);
        web_tab_free(&tab);
        return NULL;
    }
    if (!web_cdp_navigate(&ws, url, err, err_len) ||
        !web_wait_navigated_ready(&ws, url, err, err_len))
    {
        web_ws_close(&ws);
        web_close_tab(web, &tab);
        web_tab_free(&tab);
        return NULL;
    }
    if (google_search && web_check_google_block(&ws, err, err_len) != 0) {
        web_ws_close(&ws);
        web_close_tab(web, &tab);
        web_tab_free(&tab);
        return NULL;
    }
    char *clicked = NULL;
    if (google_search) {
        clicked = web_cdp_eval_string(
            &ws, web_click_google_consent_js, err, err_len);
        if (!clicked) {
            web_ws_close(&ws);
            web_close_tab(web, &tab);
            web_tab_free(&tab);
            return NULL;
        }
        if (clicked[0]) {
            web_log(web, clicked);
            if (!web_wait_navigated_ready(&ws, url, err, err_len) ||
                web_check_google_block(&ws, err, err_len) != 0) {
                free(clicked);
                web_ws_close(&ws);
                web_close_tab(web, &tab);
                web_tab_free(&tab);
                return NULL;
            }
        }
    }
    free(clicked);
    if (dynamic_scroll && !web_scroll_dynamic_page(&ws, err, err_len)) {
        web_ws_close(&ws);
        web_close_tab(web, &tab);
        web_tab_free(&tab);
        return NULL;
    }
    char *out = web_cdp_eval_string(&ws, js, err, err_len);
    web_ws_close(&ws);
    web_close_tab(web, &tab);
    web_tab_free(&tab);
    return out;
}

sirio_tool_web *sirio_tool_web_create(const sirio_tool_web_options *cfg) {
    sirio_tool_web *web = web_xmalloc(sizeof(*web));
    memset(web, 0, sizeof(*web));
    const char *home = cfg && cfg->home_dir && cfg->home_dir[0] ?
        cfg->home_dir : getenv("HOME");
    if (!home || !home[0]) home = ".";
    snprintf(web->home, sizeof(web->home), "%s", home);
    snprintf(web->profile_dir, sizeof(web->profile_dir), "%s/.sirio/browser", home);
    web->port = cfg && cfg->port > 0 ? cfg->port : WORKER_WEB_DEFAULT_PORT;
    web->chrome_pid = 0;
    web->next_cdp_id = 1;
    if (cfg) {
        web->confirm = cfg->confirm;
        web->confirm_private_data = cfg->confirm_private_data;
        web->log = cfg->log;
        web->log_private_data = cfg->log_private_data;
        web->cancel = cfg->cancel;
        web->cancel_private_data = cfg->cancel_private_data;
    }
    return web;
}

void sirio_tool_web_free(sirio_tool_web *web) {
    if (!web) return;
    web_stop_chrome(web);
    free(web);
}

char *sirio_tool_web_google_search(sirio_tool_web *web, const char *query,
                                      char *err, size_t err_len) {
    if (!web) {
        web_set_err(err, err_len, "web subsystem is not initialized");
        return NULL;
    }
    if (!query || !query[0]) {
        web_set_err(err, err_len, "google_search requires query");
        return NULL;
    }
    char *q = web_url_encode(query);
    web_buf url = {0};
    web_buf_puts(&url, "https://www.google.com/search?q=");
    web_buf_puts(&url, q);
    free(q);
    char *url_s = web_buf_take(&url);
    char *out = web_run_page_js(
        web, url_s, web_extract_search_js, false, true, err, err_len);
    free(url_s);
    return out;
}

char *sirio_tool_web_visit_page(sirio_tool_web *web, const char *url,
                                   char *err, size_t err_len) {
    if (!web) {
        web_set_err(err, err_len, "web subsystem is not initialized");
        return NULL;
    }
    if (!url || !url[0]) {
        web_set_err(err, err_len, "visit_page requires url");
        return NULL;
    }
    return web_run_page_js(
        web, url, web_extract_page_js, true, false, err, err_len);
}
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
    struct {
        int ctx_size;
    } gen;
    bool edit_upto;
    void *external_tools;
} agent_config;

typedef struct agent_bash_job agent_bash_job;

typedef struct {
    agent_config *cfg;
    char more_path[PATH_MAX];
    int more_next_line;
    bool more_bare;
    bool more_valid;
    int next_bash_job_id;
    agent_bash_job *bash_jobs;
    pthread_mutex_t mu;
    bool raw_mode_needs_restore;
    sirio_tool_web *web;
    sirio_tool_log_fn log;
    void *log_private_data;
} agent_worker;

struct sirio_tool_runtime {
    agent_config config;
    agent_worker worker;
};

static void *xmalloc(size_t size) {
    void *pointer = malloc(size ? size : 1);
    if (!pointer) {
        perror("sirio-tool-runner: malloc");
        exit(1);
    }
    return pointer;
}

static void *xrealloc(void *pointer, size_t size) {
    void *replacement = realloc(pointer, size ? size : 1);
    if (!replacement) {
        perror("sirio-tool-runner: realloc");
        exit(1);
    }
    return replacement;
}

static char *xstrdup(const char *text) {
    if (!text) text = "";
    size_t size = strlen(text) + 1;
    char *copy = xmalloc(size);
    memcpy(copy, text, size);
    return copy;
}

static char *xstrndup(const char *text, size_t length) {
    char *copy = xmalloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void write_all(int fd, const char *text, size_t length) {
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        text += written;
        length -= (size_t)written;
    }
}

static double now_sec(void) {
    struct timespec time_value;
    clock_gettime(CLOCK_MONOTONIC, &time_value);
    return (double)time_value.tv_sec +
           (double)time_value.tv_nsec * 1.0e-9;
}

static int set_nonblock(int fd, bool on, int *old_flags) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (old_flags) *old_flags = flags;
    int next = on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, next);
}

static void agent_buf_append(agent_buf *buffer,
                             const char *text, size_t length) {
    if (!length || buffer->truncated) return;
    const size_t maximum = 128 * 1024;
    if (buffer->len + length > maximum) {
        length = maximum > buffer->len ? maximum - buffer->len : 0;
        buffer->truncated = true;
    }
    if (!length) return;
    if (buffer->len + length + 1 > buffer->cap) {
        size_t capacity = buffer->cap ? buffer->cap * 2 : 4096;
        while (capacity < buffer->len + length + 1) capacity *= 2;
        buffer->ptr = xrealloc(buffer->ptr, capacity);
        buffer->cap = capacity;
    }
    memcpy(buffer->ptr + buffer->len, text, length);
    buffer->len += length;
    buffer->ptr[buffer->len] = '\0';
}

static void agent_buf_puts(agent_buf *buffer, const char *text) {
    agent_buf_append(buffer, text, strlen(text));
}

static char *agent_buf_take(agent_buf *buffer) {
    if (!buffer->ptr) return xstrdup("");
    char *result = buffer->ptr;
    memset(buffer, 0, sizeof(*buffer));
    return result;
}

static void agent_tool_call_free(agent_tool_call *call) {
    if (!call) return;
    free(call->name);
    for (int i = 0; i < call->argc; i++) {
        free(call->args[i].name);
        free(call->args[i].value);
    }
    free(call->args);
    memset(call, 0, sizeof(*call));
}

static void agent_tool_call_add_arg(agent_tool_call *call, const char *name,
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

static const char *agent_tool_arg_value(const agent_tool_call *call,
                                        const char *name) {
    for (int i = 0; i < call->argc; i++)
        if (call->args[i].name && !strcmp(call->args[i].name, name))
            return call->args[i].value ? call->args[i].value : "";
    return NULL;
}

static bool agent_mkdir_p(const char *path) {
    if (!path || !path[0]) return false;
    char *copy = xstrdup(path);
    for (char *cursor = copy + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
            free(copy);
            return false;
        }
        *cursor = '/';
    }
    bool okay = mkdir(copy, 0700) == 0 || errno == EEXIST;
    free(copy);
    return okay;
}

static int agent_worker_effective_ctx_size(const agent_worker *worker) {
    return worker && worker->cfg ? worker->cfg->gen.ctx_size : 0;
}

static bool worker_should_interrupt(agent_worker *worker) {
    (void)worker;
    return false;
}

static void agent_publish(agent_worker *worker,
                          const char *text, size_t length) {
    (void)worker;
    (void)text;
    (void)length;
}

static void agent_publishf_system_status(agent_worker *worker,
                                         const char *format, ...) {
    if (!worker || !worker->log || !format) return;
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    worker->log(worker->log_private_data, message);
}

static char *worker_execute_external_tool(agent_worker *worker,
                                          const agent_tool_call *call) {
    (void)worker;
    (void)call;
    return xstrdup("Tool error: nested external tool execution is disabled\n");
}

static char *worker_tool_google_search(agent_worker *worker,
                                       const agent_tool_call *call);
static char *worker_tool_visit_page(agent_worker *worker,
                                    const agent_tool_call *call);

/* ============================================================================
 * Tool Argument Parsing And File Tool Helpers
 * ============================================================================
 */


static int agent_parse_timeout(const char *s) {
    if (!s || !s[0]) return 3600;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0.0 || !isfinite(v)) return 3600;
    if (v < 1.0) v = 1.0;
    if (v > 24.0 * 3600.0) v = 24.0 * 3600.0;
    return (int)v;
}

static int agent_parse_int_default(const char *s, int def, int min, int max) {
    if (!s || !s[0]) return def;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return def;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return def;
    if (v < min) v = min;
    if (v > max) v = max;
    return (int)v;
}

static bool agent_parse_bool_default(const char *s, bool def) {
    if (!s || !s[0]) return def;
    if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcmp(s, "1"))
        return true;
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcmp(s, "0"))
        return false;
    return def;
}

#define AGENT_FILE_MAX_BYTES (16*1024*1024)
#define AGENT_READ_DEFAULT_LINES_SMALL 120
#define AGENT_READ_DEFAULT_LINES_MEDIUM 240
#define AGENT_READ_DEFAULT_LINES_LARGE 500
#define AGENT_READ_SMALL_CONTEXT_MAX 8192
#define AGENT_READ_MEDIUM_CONTEXT_MAX 16384
#define AGENT_TOOL_RESULT_RESERVE_TOKENS 1024
#define AGENT_EDIT_UPTO_MIN_PREFIX_BYTES 64
#define AGENT_EDIT_UPTO_MIN_PREFIX_LINES 2
#define AGENT_COMPACT_SOFT_PERCENT 85
#define AGENT_COMPACT_MIN_FREE_TOKENS 8192
typedef struct {
    size_t start;
    size_t content_end;
    size_t end;
} agent_line_span;

typedef struct {
    agent_line_span *v;
    int len;
    int cap;
} agent_line_spans;

static void agent_line_spans_free(agent_line_spans *spans) {
    free(spans->v);
    memset(spans, 0, sizeof(*spans));
}

static void agent_line_spans_push(agent_line_spans *spans, agent_line_span span) {
    if (spans->len == spans->cap) {
        spans->cap = spans->cap ? spans->cap * 2 : 128;
        spans->v = xrealloc(spans->v, (size_t)spans->cap * sizeof(spans->v[0]));
    }
    spans->v[spans->len++] = span;
}

/* Split a text buffer into line spans.  content_end excludes CR/LF so callers
 * can print or compare line content without newline spelling differences. */
static void agent_split_lines(const char *data, size_t len, agent_line_spans *spans) {
    size_t pos = 0;
    while (pos < len) {
        size_t start = pos;
        while (pos < len && data[pos] != '\n' && data[pos] != '\r') pos++;
        size_t content_end = pos;
        if (pos < len) {
            if (data[pos] == '\r' && pos + 1 < len && data[pos + 1] == '\n')
                pos += 2;
            else
                pos++;
        }
        agent_line_spans_push(spans, (agent_line_span){
            .start = start,
            .content_end = content_end,
            .end = pos,
        });
    }
}

static int agent_read_file_bytes(const char *path, char **data, size_t *len,
                                 char *err, size_t errlen) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }
    char *buf = NULL;
    size_t used = 0, cap = 0;
    char tmp[8192];
    while (true) {
        size_t n = fread(tmp, 1, sizeof(tmp), fp);
        if (n) {
            if (used + n > AGENT_FILE_MAX_BYTES) {
                fclose(fp);
                free(buf);
                snprintf(err, errlen, "file too large: %s exceeds %d bytes",
                         path, AGENT_FILE_MAX_BYTES);
                return -1;
            }
            if (used + n + 1 > cap) {
                cap = cap ? cap * 2 : 8192;
                while (cap < used + n + 1) cap *= 2;
                buf = xrealloc(buf, cap);
            }
            memcpy(buf + used, tmp, n);
            used += n;
            buf[used] = '\0';
        }
        if (n < sizeof(tmp)) {
            if (ferror(fp)) {
                snprintf(err, errlen, "read %s: %s", path, strerror(errno));
                fclose(fp);
                free(buf);
                return -1;
            }
            break;
        }
    }
    fclose(fp);
    if (!buf) buf = xstrdup("");
    *data = buf;
    *len = used;
    return 0;
}

static int agent_line_for_offset(const agent_line_spans *spans, size_t offset) {
    if (!spans || spans->len <= 0) return 1;
    for (int i = 0; i < spans->len; i++) {
        if (offset < spans->v[i].end) return i + 1;
    }
    return spans->len;
}

static bool agent_old_new_line_effect(const char *old_data, size_t old_len,
                                      const char *new_data, size_t new_len,
                                      size_t edit_offset, size_t replaced_len,
                                      int *start_line, int *end_line,
                                      int *delta) {
    agent_line_spans old_spans = {0};
    agent_line_spans new_spans = {0};
    agent_split_lines(old_data, old_len, &old_spans);
    agent_split_lines(new_data, new_len, &new_spans);
    bool ok = old_spans.len > 0;
    if (ok) {
        size_t old_last = edit_offset;
        if (replaced_len > 0) old_last = edit_offset + replaced_len - 1;
        if (old_last >= old_len) old_last = old_len ? old_len - 1 : 0;
        if (start_line) *start_line = agent_line_for_offset(&old_spans, edit_offset);
        if (end_line) *end_line = agent_line_for_offset(&old_spans, old_last);
        if (delta) *delta = new_spans.len - old_spans.len;
    }
    agent_line_spans_free(&old_spans);
    agent_line_spans_free(&new_spans);
    return ok;
}

static void agent_edit_result_append_context(agent_buf *b,
                                             const char *path,
                                             const char *data, size_t len,
                                             int anchor_start,
                                             int anchor_end);

static char *agent_edit_result(const char *path,
                                       int start_line, int end_line, int delta,
                                       const char *new_data, size_t new_len,
                                       const char *kind) {
    agent_buf b = {0};
    char msg[PATH_MAX + 180];
    snprintf(msg, sizeof(msg), "Edited %s using %s\n", path, kind);
    agent_buf_puts(&b, msg);
    if (start_line > 0 && end_line >= start_line) {
        snprintf(msg, sizeof(msg),
                 "Touched old lines %d-%d; current post-edit context follows.\n",
                 start_line, end_line);
        agent_buf_puts(&b, msg);
        if (delta != 0) {
            snprintf(msg, sizeof(msg),
                     "Line shift: old lines after %d moved by %+d (old line %d is now line %d). Re-read before relying on old line numbers there.\n",
                     end_line, delta, end_line + 1, end_line + 1 + delta);
            agent_buf_puts(&b, msg);
        }
    }
    if (start_line > 0 && end_line >= start_line) {
        int new_anchor_end = end_line + delta;
        if (new_anchor_end < start_line) new_anchor_end = start_line;
        agent_edit_result_append_context(&b, path, new_data, new_len,
                                         start_line, new_anchor_end);
    }
    return agent_buf_take(&b);
}

static void agent_worker_set_more(agent_worker *w, const char *path,
                                  int next_line, bool bare) {
    snprintf(w->more_path, sizeof(w->more_path), "%s", path ? path : "");
    w->more_next_line = next_line;
    w->more_bare = bare;
    w->more_valid = path && path[0] && next_line > 0;
}

int agent_tool_result_reserve_tokens(agent_worker *w) {
    int ctx = agent_worker_effective_ctx_size(w);
    int reserve = AGENT_TOOL_RESULT_RESERVE_TOKENS;
    if (ctx > 0) {
        int proportional = ctx / 8;
        if (proportional < 16) proportional = 16;
        if (reserve > proportional) reserve = proportional;
    }
    return reserve;
}

static int agent_read_default_lines(agent_worker *w) {
    int ctx = agent_worker_effective_ctx_size(w);
    if (ctx > 0 && ctx <= AGENT_READ_SMALL_CONTEXT_MAX)
        return AGENT_READ_DEFAULT_LINES_SMALL;
    if (ctx > 0 && ctx <= AGENT_READ_MEDIUM_CONTEXT_MAX)
        return AGENT_READ_DEFAULT_LINES_MEDIUM;
    return AGENT_READ_DEFAULT_LINES_LARGE;
}

/* Read file text for the model.  Normal mode shows plain line numbers.  Raw
 * mode is reserved for cases where line decoration would corrupt the payload
 * being inspected. */
static char *agent_read_range(agent_worker *w, const char *path, int start_line,
                              int max_lines, bool whole_file, bool bare,
                              bool set_more) {
    char err[256];
    char *data = NULL;
    size_t len = 0;
    if (!path || !path[0]) return xstrdup("Tool error: read requires path\n");
    if (agent_read_file_bytes(path, &data, &len, err, sizeof(err)) != 0) {
        agent_buf b = {0};
        agent_buf_puts(&b, "Tool error: ");
        agent_buf_puts(&b, err);
        agent_buf_puts(&b, "\n");
        return agent_buf_take(&b);
    }

    agent_line_spans spans = {0};
    agent_split_lines(data, len, &spans);
    if (start_line < 1) start_line = 1;
    int start_idx = start_line - 1;
    if (start_idx > spans.len) start_idx = spans.len;
    if (whole_file) {
        max_lines = spans.len - start_idx;
    } else {
        if (max_lines <= 0) max_lines = agent_read_default_lines(w);
    }
    int end_idx = start_idx + max_lines;
    if (end_idx > spans.len) end_idx = spans.len;

    agent_buf out = {0};
    if (bare) {
        size_t start = start_idx < spans.len ? spans.v[start_idx].start : len;
        size_t end = end_idx > start_idx ? spans.v[end_idx - 1].end : start;
        agent_buf_append(&out, data + start, end - start);
        if (end > start && out.ptr[out.len - 1] != '\n') agent_buf_puts(&out, "\n");
        if (end_idx < spans.len) {
            char note[160];
            snprintf(note, sizeof(note),
                     "[Read truncated at line %d of %d. continue_offset=%d. "
                     "Call more with count=%d to read the next chunk.]\n",
                     end_idx, spans.len, end_idx + 1,
                     max_lines > 0 ? max_lines : agent_read_default_lines(w));
            agent_buf_puts(&out, note);
        }
    } else {
        char hdr[PATH_MAX + 160];
        if (end_idx < spans.len) {
            snprintf(hdr, sizeof(hdr),
                     "%s: lines %d-%d of %d; continue_offset=%d; "
                     "call more with count=%d to read the next chunk\n",
                     path, spans.len ? start_idx + 1 : 0, end_idx, spans.len,
                     end_idx + 1, max_lines > 0 ? max_lines : agent_read_default_lines(w));
        } else {
            snprintf(hdr, sizeof(hdr), "%s: lines %d-%d of %d\n",
                     path, spans.len ? start_idx + 1 : 0, end_idx, spans.len);
        }
        agent_buf_puts(&out, hdr);
        for (int i = start_idx; i < end_idx; i++) {
            agent_line_span sp = spans.v[i];
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%d ", i + 1);
            agent_buf_puts(&out, prefix);
            agent_buf_append(&out, data + sp.start, sp.content_end - sp.start);
            agent_buf_puts(&out, "\n");
        }
    }
    if (set_more) {
        if (end_idx < spans.len) agent_worker_set_more(w, path, end_idx + 1, bare);
        else agent_worker_set_more(w, NULL, 0, false);
    }
    agent_line_spans_free(&spans);
    free(data);
    return agent_buf_take(&out);
}

static char *agent_tool_read(agent_worker *w, const agent_tool_call *call) {
    const char *path = agent_tool_arg_value(call, "path");
    bool whole = agent_parse_bool_default(agent_tool_arg_value(call, "whole"), false);
    int start = agent_parse_int_default(agent_tool_arg_value(call, "start_line"),
                                        1, 1, INT_MAX);
    int count = agent_parse_int_default(agent_tool_arg_value(call, "max_lines"),
                                        agent_read_default_lines(w), 1, INT_MAX);
    bool raw = agent_parse_bool_default(agent_tool_arg_value(call, "raw"), false);
    return agent_read_range(w, path, start, count, whole, raw, true);
}

static char *agent_tool_more(agent_worker *w, const agent_tool_call *call) {
    int count = agent_parse_int_default(agent_tool_arg_value(call, "count"),
                                        agent_read_default_lines(w), 1, INT_MAX);
    if (!w->more_valid) return xstrdup("Tool error: no previous output to continue\n");
    return agent_read_range(w, w->more_path, w->more_next_line, count, false,
                            w->more_bare, true);
}

static char *agent_tool_write(agent_worker *w, const agent_tool_call *call) {
    (void)w;
    const char *path = agent_tool_arg_value(call, "path");
    const char *content = agent_tool_arg_value(call, "content");
    if (!path || !path[0]) return xstrdup("Tool error: write requires path\n");
    if (!content) return xstrdup("Tool error: write requires content\n");
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        agent_buf b = {0};
        agent_buf_puts(&b, "Tool error: open for write failed: ");
        agent_buf_puts(&b, strerror(errno));
        agent_buf_puts(&b, "\n");
        return agent_buf_take(&b);
    }
    size_t len = strlen(content);
    size_t wr = fwrite(content, 1, len, fp);
    int close_rc = fclose(fp);
    if (wr != len || close_rc != 0) {
        agent_buf b = {0};
        agent_buf_puts(&b, "Tool error: write failed: ");
        agent_buf_puts(&b, strerror(errno));
        agent_buf_puts(&b, "\n");
        return agent_buf_take(&b);
    }
    char msg[PATH_MAX + 160];
    snprintf(msg, sizeof(msg), "Wrote %zu bytes to %s\n", len, path);
    return xstrdup(msg);
}

static char *agent_tool_list(const agent_tool_call *call) {
    const char *path = agent_tool_arg_value(call, "path");
    if (!path || !path[0]) path = ".";
    DIR *dir = opendir(path);
    if (!dir) {
        agent_buf b = {0};
        agent_buf_puts(&b, "Tool error: opendir failed: ");
        agent_buf_puts(&b, strerror(errno));
        agent_buf_puts(&b, "\n");
        return agent_buf_take(&b);
    }
    agent_buf out = {0};
    char hdr[PATH_MAX + 64];
    snprintf(hdr, sizeof(hdr), "%s:\n", path);
    agent_buf_puts(&out, hdr);
    struct dirent *de;
    int shown = 0;
    while ((de = readdir(dir)) != NULL && shown < 300) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        char type = S_ISDIR(st.st_mode) ? 'd' :
                    S_ISLNK(st.st_mode) ? 'l' :
                    S_ISREG(st.st_mode) ? '-' : '?';
        char line[PATH_MAX + 96];
        snprintf(line, sizeof(line), "%c %10lld %s%s\n", type,
                 (long long)st.st_size, de->d_name, S_ISDIR(st.st_mode) ? "/" : "");
        agent_buf_puts(&out, line);
        shown++;
    }
    if (de) agent_buf_puts(&out, "... more entries omitted ...\n");
    closedir(dir);
    return agent_buf_take(&out);
}

/* ============================================================================
 * Edit And Search Tools
 * ============================================================================
 */

static int agent_write_file_bytes(const char *path, const char *data, size_t len,
                                  char *err, size_t errlen) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }
    size_t wr = fwrite(data, 1, len, fp);
    if (wr != len) {
        snprintf(err, errlen, "write %s: %s", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) {
        snprintf(err, errlen, "close %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void agent_edit_result_append_line(agent_buf *b, const char *data,
                                          const agent_line_span *sp,
                                          int line) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%d ", line);
    agent_buf_puts(b, prefix);
    agent_buf_append(b, data + sp->start, sp->content_end - sp->start);
    agent_buf_puts(b, "\n");
}

/* Successful edits return the nearby post-edit file shape.  This spends cheap
 * prefill tokens to save expensive model retries: the model immediately sees
 * shifted line numbers, braces, semicolons, and accidental duplication. */
static void agent_edit_result_append_context(agent_buf *b,
                                             const char *path,
                                             const char *data, size_t len,
                                             int anchor_start,
                                             int anchor_end) {
    enum {
        CONTEXT_BEFORE = 5,
        CONTEXT_AFTER = 8,
        EDITED_CONTEXT_HEAD = 18,
        EDITED_CONTEXT_TAIL = 18
    };

    agent_line_spans spans = {0};
    agent_split_lines(data, len, &spans);
    if (spans.len <= 0) {
        agent_line_spans_free(&spans);
        return;
    }

    if (anchor_start < 1) anchor_start = 1;
    if (anchor_start > spans.len) anchor_start = spans.len;
    if (anchor_end < anchor_start) anchor_end = anchor_start;
    if (anchor_end > spans.len) anchor_end = spans.len;

    int ctx_start = anchor_start - CONTEXT_BEFORE;
    if (ctx_start < 1) ctx_start = 1;
    int ctx_end = anchor_end + CONTEXT_AFTER;
    if (ctx_end > spans.len) ctx_end = spans.len;

    char hdr[PATH_MAX + 160];
    snprintf(hdr, sizeof(hdr),
             "Current file around edit: %s lines %d-%d of %d\n",
             path, ctx_start, ctx_end, spans.len);
    agent_buf_puts(b, hdr);

    int edited_lines = anchor_end - anchor_start + 1;
    if (edited_lines <= EDITED_CONTEXT_HEAD + EDITED_CONTEXT_TAIL) {
        for (int line = ctx_start; line <= ctx_end; line++)
            agent_edit_result_append_line(b, data, &spans.v[line - 1], line);
    } else {
        int head_end = anchor_start + EDITED_CONTEXT_HEAD - 1;
        int tail_start = anchor_end - EDITED_CONTEXT_TAIL + 1;
        for (int line = ctx_start; line <= head_end; line++)
            agent_edit_result_append_line(b, data, &spans.v[line - 1], line);
        snprintf(hdr, sizeof(hdr),
                 "... %d edited lines omitted ...\n",
                 tail_start - head_end - 1);
        agent_buf_puts(b, hdr);
        for (int line = tail_start; line <= ctx_end; line++)
            agent_edit_result_append_line(b, data, &spans.v[line - 1], line);
    }

    agent_line_spans_free(&spans);
}

static const char *agent_memmem_simple(const char *hay, size_t hay_len,
                                       const char *needle, size_t needle_len) {
    if (!needle_len) return hay;
    if (needle_len > hay_len) return NULL;
    size_t last = hay_len - needle_len;
    for (size_t i = 0; i <= last; i++) {
        if (hay[i] == needle[0] && !memcmp(hay + i, needle, needle_len))
            return hay + i;
    }
    return NULL;
}

static bool agent_find_unique(const char *data, size_t len,
                              const char *needle, size_t needle_len,
                              const char **match, const char *label,
                              char *err, size_t err_len) {
    if (!needle || needle_len == 0) {
        snprintf(err, err_len, "%s anchor is empty", label);
        return false;
    }
    const char *first = agent_memmem_simple(data, len, needle, needle_len);
    if (!first) {
        snprintf(err, err_len, "%s anchor not found", label);
        return false;
    }
    size_t after_first = (size_t)(first - data) + 1;
    const char *second = after_first <= len ?
        agent_memmem_simple(data + after_first, len - after_first,
                            needle, needle_len) : NULL;
    if (second) {
        snprintf(err, err_len, "%s anchor is not unique", label);
        return false;
    }
    *match = first;
    return true;
}

/* Find an anchor only in the suffix after start.
 *
 * Anchored edits use "head [upto] tail": the head fixes the edit start, and
 * the tail should delimit the first unique end point after that start. A tail
 * may legitimately appear earlier in the file, so checking global uniqueness
 * would reject valid edits.
 */
static bool agent_find_unique_after(const char *data, size_t len,
                                    const char *start,
                                    const char *needle, size_t needle_len,
                                    const char **match, const char *label,
                                    char *err, size_t err_len) {
    if (!needle || needle_len == 0) {
        snprintf(err, err_len, "%s anchor is empty", label);
        return false;
    }
    if (start < data || start > data + len) {
        snprintf(err, err_len, "%s search starts outside file", label);
        return false;
    }
    size_t off = (size_t)(start - data);
    const char *first = agent_memmem_simple(data + off, len - off,
                                            needle, needle_len);
    if (!first) {
        snprintf(err, err_len, "%s anchor not found after old head", label);
        return false;
    }
    size_t after_first = (size_t)(first - data) + 1;
    const char *second = after_first <= len ?
        agent_memmem_simple(data + after_first, len - after_first,
                            needle, needle_len) : NULL;
    if (second) {
        snprintf(err, err_len, "%s anchor is not unique after old head", label);
        return false;
    }
    *match = first;
    return true;
}

static bool agent_span_has_nonspace(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)s[i])) return true;
    }
    return false;
}

static bool agent_edit_find_old_span(const char *data, size_t len,
                                     const char *old, bool allow_upto,
                                     const char **match,
                                     size_t *match_len, bool *anchored,
                                     char *err, size_t err_len) {
    static const char marker[] = "[upto]";
    size_t old_len = strlen(old);
    const char *upto = strstr(old, marker);
    if (!upto) {
        *anchored = false;
        if (!agent_find_unique(data, len, old, old_len, match, "old text",
                               err, err_len))
            return false;
        *match_len = old_len;
        return true;
    }
    if (!allow_upto) {
        snprintf(err, err_len,
                 "[upto] edits are disabled; restart with --edit-upto to enable them");
        return false;
    }
    if (strstr(upto + strlen(marker), marker)) {
        snprintf(err, err_len, "old text contains more than one [upto] marker");
        return false;
    }
    size_t head_len = (size_t)(upto - old);
    const char *tail = upto + strlen(marker);
    size_t tail_len = old_len - head_len - strlen(marker);
    /* Strip leading newline/CR from tail before searching.  The head already
     * includes the newline at its end, so the extra \n that follows [upto] in
     * the old text (whether injected by the forcer or written by the model)
     * must not be part of the tail needle -- the file after the head has no
     * duplicate newline. */
    while (tail_len > 0 && (*tail == '\n' || *tail == '\r')) {
        tail++;
        tail_len--;
    }
    if (!agent_span_has_nonspace(tail, tail_len)) {
        snprintf(err, err_len,
                 "old text after [upto] must include a unique tail anchor");
        return false;
    }
    const char *head_pos = NULL;
    const char *tail_pos = NULL;
    if (!agent_find_unique(data, len, old, head_len, &head_pos, "old head",
                           err, err_len))
        return false;
    if (!agent_find_unique_after(data, len, head_pos + head_len,
                                 tail, tail_len, &tail_pos, "old tail",
                                 err, err_len))
        return false;
    *anchored = true;
    *match = head_pos;
    *match_len = (size_t)(tail_pos - head_pos) + tail_len;
    return true;
}

static char *agent_apply_file_splice(const char *path,
                                     const char *data, size_t len,
                                     size_t offset, size_t remove_len,
                                     const char *insert, const char *kind) {
    char err[256];
    if (!insert) insert = "";
    size_t insert_len = strlen(insert);
    size_t out_len = offset + insert_len + (len - offset - remove_len);
    char *out = xmalloc(out_len + 1);
    memcpy(out, data, offset);
    memcpy(out + offset, insert, insert_len);
    memcpy(out + offset + insert_len, data + offset + remove_len,
           len - offset - remove_len);
    out[out_len] = '\0';

    int rc = agent_write_file_bytes(path, out, out_len, err, sizeof(err));
    if (rc != 0) {
        free(out);
        agent_buf b = {0};
        agent_buf_puts(&b, "Tool error: ");
        agent_buf_puts(&b, err);
        agent_buf_puts(&b, "\n");
        return agent_buf_take(&b);
    }

    int start_line = 0, end_line = 0, delta = 0;
    agent_old_new_line_effect(data, len, out, out_len, offset, remove_len,
                              &start_line, &end_line, &delta);
    char *result = agent_edit_result(path, start_line, end_line, delta,
                                     out, out_len, kind);
    free(out);
    return result;
}

/* Old/new editing is intentionally conservative: exact old text must be unique.
 * For large replacements, old may contain one [upto] marker: the head must be
 * unique, and the tail must be unique after that head before the whole span is
 * replaced. */
static char *agent_tool_edit(agent_worker *w, const agent_tool_call *call) {
    const char *path = agent_tool_arg_value(call, "path");
    if (!path || !path[0]) return xstrdup("Tool error: edit requires path\n");
    const char *old = agent_tool_arg_value(call, "old");
    const char *new_text = agent_tool_arg_value(call, "new");
    if (!old || !old[0]) return xstrdup("Tool error: edit requires non-empty old text\n");
    if (!new_text) return xstrdup("Tool error: edit requires new text\n");

    char err[256];
    char *data = NULL;
    size_t len = 0;
    if (agent_read_file_bytes(path, &data, &len, err, sizeof(err)) != 0) {
        agent_buf b = {0};
        agent_buf_puts(&b, "Tool error: ");
        agent_buf_puts(&b, err);
        agent_buf_puts(&b, "\n");
        return agent_buf_take(&b);
    }

    const char *match = NULL;
    size_t match_len = 0;
    bool anchored = false;
    bool allow_upto = w && w->cfg && w->cfg->edit_upto;
    if (!agent_edit_find_old_span(data, len, old, allow_upto,
                                  &match, &match_len,
                                  &anchored, err, sizeof(err)))
    {
        free(data);
        agent_buf b = {0};
        agent_buf_puts(&b, "Tool error: ");
        agent_buf_puts(&b, err);
        agent_buf_puts(&b, "\n");
        return agent_buf_take(&b);
    }

    char *result = agent_apply_file_splice(path, data, len,
                                           (size_t)(match - data), match_len,
                                           new_text,
                                           anchored ? "anchored old/new replacement"
                                                    : "old/new replacement");
    free(data);
    return result;
}

typedef struct {
    const char *query;
    const char *glob;
    regex_t regex;
    bool use_regex;
    bool regex_ready;
    bool case_sensitive;
    int context;
    int max_results;
    int results;
    agent_buf out;
} agent_search_ctx;

static bool agent_literal_match(const char *s, size_t n, const char *q,
                                bool case_sensitive) {
    size_t qn = strlen(q);
    if (!qn) return true;
    if (qn > n) return false;
    for (size_t i = 0; i + qn <= n; i++) {
        bool ok = true;
        for (size_t j = 0; j < qn; j++) {
            unsigned char a = (unsigned char)s[i + j];
            unsigned char b = (unsigned char)q[j];
            if (!case_sensitive) {
                a = (unsigned char)tolower(a);
                b = (unsigned char)tolower(b);
            }
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

static bool agent_search_line_matches(agent_search_ctx *ctx, const char *s, size_t n) {
    if (ctx->use_regex) {
        char *line = xstrndup(s, n);
        int rc = regexec(&ctx->regex, line, 0, NULL, 0);
        free(line);
        return rc == 0;
    }
    return agent_literal_match(s, n, ctx->query, ctx->case_sensitive);
}

static void agent_search_emit_line(agent_search_ctx *ctx, const char *data,
                                   agent_line_span sp, int line_no) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "  %d ", line_no);
    agent_buf_puts(&ctx->out, prefix);
    agent_buf_append(&ctx->out, data + sp.start, sp.content_end - sp.start);
    agent_buf_puts(&ctx->out, "\n");
}

/* Search one text file and emit matching lines with plain line numbers. */
static void agent_search_file(agent_search_ctx *ctx, const char *path) {
    if (ctx->results >= ctx->max_results) return;
    if (ctx->glob && ctx->glob[0]) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (fnmatch(ctx->glob, base, 0) != 0 && fnmatch(ctx->glob, path, 0) != 0)
            return;
    }
    char err[256];
    char *data = NULL;
    size_t len = 0;
    if (agent_read_file_bytes(path, &data, &len, err, sizeof(err)) != 0) return;
    if (memchr(data, '\0', len)) {
        free(data);
        return;
    }
    agent_line_spans spans = {0};
    agent_split_lines(data, len, &spans);
    bool printed_file = false;
    int last_context_line = -1;
    for (int i = 0; i < spans.len && ctx->results < ctx->max_results; i++) {
        agent_line_span sp = spans.v[i];
        if (!agent_search_line_matches(ctx, data + sp.start, sp.content_end - sp.start))
            continue;
        if (!printed_file) {
            agent_buf_puts(&ctx->out, path);
            agent_buf_puts(&ctx->out, "\n");
            printed_file = true;
        }
        int from = i - ctx->context;
        int to = i + ctx->context;
        if (from < 0) from = 0;
        if (to >= spans.len) to = spans.len - 1;
        if (from <= last_context_line) from = last_context_line + 1;
        for (int j = from; j <= to; j++) {
            agent_search_emit_line(ctx, data, spans.v[j], j + 1);
            last_context_line = j;
        }
        ctx->results++;
    }
    if (printed_file) agent_buf_puts(&ctx->out, "\n");
    agent_line_spans_free(&spans);
    free(data);
}

/* Recursively search a file or directory, avoiding .git and stopping once the
 * result cap is reached. */
static void agent_search_path(agent_search_ctx *ctx, const char *path, int depth) {
    if (ctx->results >= ctx->max_results || depth > 24) return;
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISREG(st.st_mode)) {
        agent_search_file(ctx, path);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && ctx->results < ctx->max_results) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strcmp(de->d_name, ".git")) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        agent_search_path(ctx, child, depth + 1);
    }
    closedir(dir);
}

/* Implement the search tool using either literal matching or POSIX regex. */
static char *agent_tool_search(agent_worker *w, const agent_tool_call *call) {
    (void)w;
    const char *query = agent_tool_arg_value(call, "query");
    if (!query || !query[0]) return xstrdup("Tool error: search requires query\n");
    const char *path = agent_tool_arg_value(call, "path");
    if (!path || !path[0]) path = ".";
    const char *mode = agent_tool_arg_value(call, "mode");
    agent_search_ctx ctx = {
        .query = query,
        .glob = agent_tool_arg_value(call, "glob"),
        .use_regex = mode && !strcmp(mode, "regex"),
        .case_sensitive = agent_parse_bool_default(agent_tool_arg_value(call, "case_sensitive"), true),
        .context = agent_parse_int_default(agent_tool_arg_value(call, "context"), 0, 0, 5),
        .max_results = agent_parse_int_default(agent_tool_arg_value(call, "max_results"), 50, 1, 500),
    };
    if (ctx.use_regex) {
        int flags = REG_EXTENDED | REG_NOSUB;
        if (!ctx.case_sensitive) flags |= REG_ICASE;
        int rc = regcomp(&ctx.regex, query, flags);
        if (rc != 0) {
            char msg[256];
            regerror(rc, &ctx.regex, msg, sizeof(msg));
            agent_buf b = {0};
            agent_buf_puts(&b, "Tool error: invalid regex: ");
            agent_buf_puts(&b, msg);
            agent_buf_puts(&b, "\n");
            return agent_buf_take(&b);
        }
        ctx.regex_ready = true;
    }
    agent_search_path(&ctx, path, 0);
    if (ctx.regex_ready) regfree(&ctx.regex);
    if (!ctx.out.ptr) agent_buf_puts(&ctx.out, "No matches\n");
    else {
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "%d match%s shown\n\n",
                 ctx.results, ctx.results == 1 ? "" : "es");
        size_t hdr_len = strlen(hdr);
        if (ctx.out.len + hdr_len + 1 > ctx.out.cap) {
            ctx.out.cap = ctx.out.len + hdr_len + 1;
            ctx.out.ptr = xrealloc(ctx.out.ptr, ctx.out.cap);
        }
        memmove(ctx.out.ptr + hdr_len, ctx.out.ptr, ctx.out.len + 1);
        memcpy(ctx.out.ptr, hdr, hdr_len);
        ctx.out.len += hdr_len;
    }
    return agent_buf_take(&ctx.out);
}

/* ============================================================================
 * Asynchronous Bash Jobs
 * ============================================================================
 *
 * Bash commands are tracked jobs, not blocking one-shot calls.  Each job owns a
 * process, a pipe, and a secure /tmp output file.  The first observation is
 * head-biased so headers and early errors are visible; later progress updates
 * are tail-biased and report how much output was added since the previous
 * observation.
 */

#define AGENT_BASH_HEAD_BYTES (8*1024)
#define AGENT_BASH_HEAD_LINES 100
#define AGENT_BASH_TAIL_BYTES (32*1024)
#define AGENT_BASH_PROGRESS_TAIL_LINES 4
#define AGENT_BASH_FINAL_TAIL_LINES 20

struct agent_bash_job {
    int id;
    pid_t pid;
    int pipe_fd;
    int tmp_fd;
    char path[PATH_MAX];
    char *cmd;
    double start_time;
    double timeout_sec;
    size_t bytes;
    int newline_count;
    char last_byte;
    size_t observed_bytes;
    int observed_display_lines;
    bool observed_once;
    int exit_status;
    bool running;
    bool timed_out;
    struct agent_bash_job *next;
    agent_worker *worker;  /* back-pointer for terminal state restoration */
};

static int agent_bash_display_lines(const agent_bash_job *job) {
    if (!job || job->bytes == 0) return 0;
    return job->newline_count + (job->last_byte != '\n');
}

static void agent_bash_note_output(agent_bash_job *job, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') job->newline_count++;
    }
    if (n) job->last_byte = s[n - 1];
    job->bytes += n;
}

static void agent_bash_job_free(agent_bash_job *job) {
    if (!job) return;
    if (job->running && job->pid > 0) {
        kill(-job->pid, SIGKILL);
        kill(job->pid, SIGKILL);
        waitpid(job->pid, NULL, 0);
    }
    if (job->pipe_fd >= 0) close(job->pipe_fd);
    if (job->tmp_fd >= 0) close(job->tmp_fd);
    free(job->cmd);
    free(job);
}

void agent_bash_jobs_free(agent_worker *w) {
    agent_bash_job *job = w->bash_jobs;
    while (job) {
        agent_bash_job *next = job->next;
        agent_bash_job_free(job);
        job = next;
    }
    w->bash_jobs = NULL;
}

static agent_bash_job *agent_bash_find_job(agent_worker *w, int id, pid_t pid) {
    for (agent_bash_job *job = w->bash_jobs; job; job = job->next) {
        if ((id > 0 && job->id == id) || (id <= 0 && pid > 0 && job->pid == pid))
            return job;
    }
    return NULL;
}

static void agent_bash_remove_job(agent_worker *w, agent_bash_job *target) {
    agent_bash_job **link = &w->bash_jobs;
    while (*link) {
        if (*link == target) {
            *link = target->next;
            target->next = NULL;
            agent_bash_job_free(target);
            return;
        }
        link = &(*link)->next;
    }
}

static void agent_bash_drain(agent_bash_job *job) {
    if (!job || job->pipe_fd < 0) return;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(job->pipe_fd, tmp, sizeof(tmp));
        if (n > 0) {
            agent_bash_note_output(job, tmp, (size_t)n);
            if (job->tmp_fd >= 0) write_all(job->tmp_fd, tmp, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

static void agent_worker_note_terminal_mode_may_have_changed(agent_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->raw_mode_needs_restore = true;
    pthread_mutex_unlock(&w->mu);
}

static void agent_bash_finalize(agent_bash_job *job, int status) {
    agent_bash_drain(job);
    if (job->pipe_fd >= 0) {
        close(job->pipe_fd);
        job->pipe_fd = -1;
    }
    if (job->tmp_fd >= 0) {
        close(job->tmp_fd);
        job->tmp_fd = -1;
    }
    if (WIFEXITED(status)) job->exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) job->exit_status = 128 + WTERMSIG(status);
    else job->exit_status = -1;
    job->running = false;
    /* A child can still open /dev/tty directly and alter terminal state even
     * though its stdin is /dev/null.  Ask the UI thread to verify raw mode at
     * a safe point instead of touching linenoise from the worker path. */
    agent_worker_note_terminal_mode_may_have_changed(job->worker);
}

/* Drain available output, notice process exit, and enforce timeout.  This is
 * called opportunistically by status/wait/compaction instead of a background
 * reaper thread, keeping all bash job state owned by the agent worker. */
static void agent_bash_poll(agent_bash_job *job) {
    if (!job || !job->running) return;
    agent_bash_drain(job);

    int status = 0;
    pid_t rc = waitpid(job->pid, &status, WNOHANG);
    if (rc == job->pid) {
        agent_bash_finalize(job, status);
        return;
    }
    if (rc < 0 && errno != EINTR) {
        job->exit_status = -1;
        job->running = false;
        if (job->pipe_fd >= 0) {
            close(job->pipe_fd);
            job->pipe_fd = -1;
        }
        if (job->tmp_fd >= 0) {
            close(job->tmp_fd);
            job->tmp_fd = -1;
        }
        agent_worker_note_terminal_mode_may_have_changed(job->worker);
        return;
    }
    if (now_sec() - job->start_time >= job->timeout_sec) {
        job->timed_out = true;
        kill(-job->pid, SIGKILL);
        kill(job->pid, SIGKILL);
        while (waitpid(job->pid, &status, 0) < 0 && errno == EINTR) {}
        agent_bash_finalize(job, status);
    }
}

/* Spawn a shell command into its own process group so bash_stop/timeout can
 * kill grandchildren created by the shell, not just the /bin/sh wrapper. */
static agent_bash_job *agent_bash_start(agent_worker *w, const char *cmd,
                                        int timeout_sec, char *err, size_t err_len) {
    char tmp_path[] = "/tmp/sirio_output_XXXXXX";
    int tmpfd = mkstemp(tmp_path);
    if (tmpfd < 0) {
        snprintf(err, err_len, "failed to create temporary output file: %s", strerror(errno));
        return NULL;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err, err_len, "failed to create pipe: %s", strerror(errno));
        close(tmpfd);
        unlink(tmp_path);
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_len, "failed to fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        close(tmpfd);
        unlink(tmp_path);
        return NULL;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(tmpfd);
        /* The bash tool is not interactive.  Give the shell /dev/null as
         * stdin so it does not inherit the live linenoise terminal and reset
         * it from raw mode to cooked mode behind the agent's back. */
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            if (dup2(null_fd, STDIN_FILENO) < 0)
                close(STDIN_FILENO);
            if (null_fd != STDIN_FILENO)
                close(null_fd);
        } else {
            close(STDIN_FILENO);
        }
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd ? cmd : "", (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    setpgid(pid, pid);
    int old_flags;
    set_nonblock(pipefd[0], true, &old_flags);

    agent_bash_job *job = xmalloc(sizeof(*job));
    memset(job, 0, sizeof(*job));
    if (w->next_bash_job_id <= 0) w->next_bash_job_id = 1;
    job->id = w->next_bash_job_id++;
    job->pid = pid;
    job->pipe_fd = pipefd[0];
    job->tmp_fd = tmpfd;
    snprintf(job->path, sizeof(job->path), "%s", tmp_path);
    job->cmd = xstrdup(cmd);
    job->start_time = now_sec();
    job->timeout_sec = timeout_sec;
    job->exit_status = -1;
    job->running = true;
    job->worker = w;
    job->next = w->bash_jobs;
    w->bash_jobs = job;
    return job;
}

static void agent_tail_append(agent_buf *b, const char *s, size_t n, size_t max) {
    if (!n) return;
    agent_buf_append(b, s, n);
    if (b->len > max) {
        size_t drop = b->len - max;
        memmove(b->ptr, b->ptr + drop, b->len - drop + 1);
        b->len -= drop;
    }
}

/* Read the first max_lines from the output file, with a byte cap to avoid a
 * pathological single long line flooding the next model turn. */
static char *agent_bash_read_head(const agent_bash_job *job, int max_lines,
                                  size_t max_bytes, int *lines_read,
                                  bool *byte_limited) {
    if (lines_read) *lines_read = 0;
    if (byte_limited) *byte_limited = false;
    if (!job || !job->path[0] || job->bytes == 0) return xstrdup("");
    FILE *fp = fopen(job->path, "rb");
    if (!fp) return xstrdup("<failed to reopen output file>\n");

    agent_buf out = {0};
    int lines = 0;
    while (lines < max_lines && out.len < max_bytes) {
        int c = fgetc(fp);
        if (c == EOF) {
            if (ferror(fp) && errno == EINTR) {
                clearerr(fp);
                continue;
            }
            break;
        }
        char ch = (char)c;
        agent_buf_append(&out, &ch, 1);
        if (ch == '\n') lines++;
    }
    if (out.len >= max_bytes && !feof(fp) && byte_limited) *byte_limited = true;
    fclose(fp);
    if (lines_read) *lines_read = lines + (out.len && out.ptr[out.len - 1] != '\n');
    if (!out.ptr) return xstrdup("");
    return agent_buf_take(&out);
}

/* Read the last max_lines from the full output file.  The model-visible label
 * says "tail -N <file>" so it is clear this is not the complete output. */
static char *agent_bash_read_tail_lines(const agent_bash_job *job, int max_lines) {
    if (!job || !job->path[0] || job->bytes == 0) return xstrdup("");
    FILE *fp = fopen(job->path, "rb");
    if (!fp) return xstrdup("<failed to reopen output file>\n");

    agent_buf tail = {0};
    char tmp[2048];
    for (;;) {
        size_t n = fread(tmp, 1, sizeof(tmp), fp);
        if (n) agent_tail_append(&tail, tmp, n, AGENT_BASH_TAIL_BYTES);
        if (n < sizeof(tmp)) {
            if (ferror(fp) && errno == EINTR) {
                clearerr(fp);
                continue;
            }
            break;
        }
    }
    fclose(fp);
    if (!tail.ptr) return xstrdup("");

    char *start = tail.ptr;
    int newlines = 0;
    for (char *p = tail.ptr + tail.len; p > tail.ptr; p--) {
        if (p[-1] == '\n' && ++newlines > max_lines) {
            start = p;
            break;
        }
    }
    char *out = xstrdup(start);
    free(tail.ptr);
    return out;
}

/* Build the tool result for a bash job.  mark_observed advances the per-job
 * cursor so the next status reports only fresh output. */
static char *agent_bash_observation(agent_bash_job *job, bool mark_observed) {
    agent_bash_poll(job);
    bool first_observation = !job->observed_once;
    int display_lines = agent_bash_display_lines(job);
    double elapsed = now_sec() - job->start_time;

    agent_buf out = {0};
    char line[PATH_MAX + 256];
    if (job->running) {
        snprintf(line, sizeof(line),
            "bash job=%d pid=%ld status=running elapsed_sec=%.1f timeout_sec=%.0f\n",
            job->id, (long)job->pid, elapsed, job->timeout_sec);
    } else {
        snprintf(line, sizeof(line),
            "bash job=%d pid=%ld status=done elapsed_sec=%.1f timed_out=%d\n",
            job->id, (long)job->pid, elapsed, job->timed_out ? 1 : 0);
    }
    agent_buf_puts(&out, line);
    if (!job->running) {
        snprintf(line, sizeof(line), "exit_status=%d\n", job->exit_status);
        agent_buf_puts(&out, line);
    }

    if (job->bytes == 0) {
        agent_buf_puts(&out, "<output>\n</output>\n");
    } else if (first_observation) {
        int shown_lines = 0;
        bool byte_limited = false;
        char *head = agent_bash_read_head(job, AGENT_BASH_HEAD_LINES,
                                          AGENT_BASH_HEAD_BYTES,
                                          &shown_lines, &byte_limited);
        bool truncated = byte_limited || display_lines > shown_lines;
        if (!job->running && !truncated) {
            agent_buf_puts(&out, "<output>\n");
            agent_buf_puts(&out, head);
            if (head[0] && head[strlen(head) - 1] != '\n') agent_buf_puts(&out, "\n");
            agent_buf_puts(&out, "</output>\n");
        } else {
            snprintf(line, sizeof(line),
                     "output_path=%s (%zu bytes, %d lines)\n",
                     job->path[0] ? job->path : "<unavailable>",
                     job->bytes, display_lines);
            agent_buf_puts(&out, line);
            snprintf(line, sizeof(line), "<head -%d %s>\n",
                     AGENT_BASH_HEAD_LINES, job->path);
            agent_buf_puts(&out, line);
            agent_buf_puts(&out, head);
            if (head[0] && head[strlen(head) - 1] != '\n') agent_buf_puts(&out, "\n");
            agent_buf_puts(&out, "</head>\n");
        }
        free(head);
    } else {
        int tail_lines = job->running ? AGENT_BASH_PROGRESS_TAIL_LINES :
                                        AGENT_BASH_FINAL_TAIL_LINES;
        char *tail = agent_bash_read_tail_lines(job, tail_lines);
        snprintf(line, sizeof(line),
                 "output_path=%s (%zu bytes, %d lines)\n",
                 job->path[0] ? job->path : "<unavailable>",
                 job->bytes, display_lines);
        agent_buf_puts(&out, line);
        snprintf(line, sizeof(line), "<tail -%d %s>\n", tail_lines, job->path);
        agent_buf_puts(&out, line);
        agent_buf_puts(&out, tail);
        if (tail[0] && tail[strlen(tail) - 1] != '\n') agent_buf_puts(&out, "\n");
        snprintf(line, sizeof(line), "</tail>\n");
        agent_buf_puts(&out, line);
        free(tail);
    }
    if (job->running) {
        snprintf(line, sizeof(line),
            "\nUse bash_status job=%d to get info before refresh time; use bash_stop job=%d to stop execution\n",
            job->id, job->id);
        agent_buf_puts(&out, line);
    }

    if (mark_observed) {
        job->observed_bytes = job->bytes;
        job->observed_display_lines = display_lines;
        job->observed_once = true;
    }
    return agent_buf_take(&out);
}

static void agent_bash_publish_observation(agent_worker *w, const char *obs) {
    if (!obs || !obs[0]) return;
    const char *body = NULL;
    const char *label = strstr(obs, "\n<head ");
    const char *close = NULL;
    if (label) {
        close = "</head>";
    } else {
        label = strstr(obs, "\n<tail ");
        if (label) close = "</tail>";
    }
    if (label) {
        const char *tag_end = strstr(label, ">\n");
        if (tag_end) {
            agent_publish(w, "\x1b[90m", 5);
            if (strstr(label, "\n<head ") == label)
                agent_publish(w, "[showing first output lines]\n",
                              strlen("[showing first output lines]\n"));
            else
                agent_publish(w, "[showing last output lines]\n",
                              strlen("[showing last output lines]\n"));
            agent_publish(w, "\x1b[0m", 4);
            body = tag_end + 2;
        }
    } else {
        label = strstr(obs, "\n<output>\n");
        if (label) {
            body = label + strlen("\n<output>\n");
            close = "</output>";
        }
    }
    if (!body || !body[0]) return;
    const char *end = close ? strstr(body, close) : NULL;
    size_t n = end ? (size_t)(end - body) : strlen(body);
    if (n) {
        bool failed = strstr(obs, "status=done") && !strstr(obs, "exit_status=0\n");
        if (failed) agent_publish(w, "\x1b[38;5;208m", 11);
        agent_publish(w, body, n);
        if (body[n - 1] != '\n') agent_publish(w, "\n", 1);
        if (failed) agent_publish(w, "\x1b[0m", 4);
    }
}

static void agent_bash_refresh_for(agent_worker *w, agent_bash_job *job,
                                   int refresh_sec) {
    double start = now_sec();
    while (job->running && now_sec() - start < refresh_sec) {
        if (worker_should_interrupt(w)) break;
        agent_bash_poll(job);
        if (!job->running) break;
        struct pollfd pfd = {.fd = job->pipe_fd, .events = POLLIN};
        poll(&pfd, 1, 100);
    }
    agent_bash_poll(job);
}

/* Common implementation for bash, bash_status, and bash_stop. */
static char *agent_bash_job_tool_result(agent_worker *w, agent_bash_job *job,
                                        bool wait, int refresh_sec,
                                        bool stop, bool remove_if_done) {
    if (stop && job->running) {
        kill(-job->pid, SIGTERM);
        kill(job->pid, SIGTERM);
        double start = now_sec();
        while (job->running && now_sec() - start < 1.0) {
            agent_bash_poll(job);
            if (!job->running) break;
            usleep(20000);
        }
        if (job->running) {
            kill(-job->pid, SIGKILL);
            kill(job->pid, SIGKILL);
        }
    }
    if (wait || stop) agent_bash_refresh_for(w, job, refresh_sec);
    else agent_bash_poll(job);

    char *obs = agent_bash_observation(job, true);
    agent_bash_publish_observation(w, obs);
    if (remove_if_done && !job->running) agent_bash_remove_job(w, job);
    return obs;
}

static int agent_tool_job_id(const agent_tool_call *call) {
    return agent_parse_int_default(agent_tool_arg_value(call, "job"), 0, 0, INT_MAX);
}

static pid_t agent_tool_pid(const agent_tool_call *call) {
    return (pid_t)agent_parse_int_default(agent_tool_arg_value(call, "pid"), 0, 0, INT_MAX);
}

/* ============================================================================
 * Tool Dispatch
 * ============================================================================
 */

/* Execute one parsed DSML tool call and return the text that will be appended as
 * the tool-role result.  UI visualization already happened while streaming; this
 * function is only about side effects and the model-visible observation. */
char *agent_execute_tool_call(agent_worker *w, const agent_tool_call *call) {
    agent_buf result = {0};
    if (!call->name) return xstrdup("Tool error: missing tool name\n");

    if (w && w->cfg && w->cfg->external_tools) {
        char *remote = worker_execute_external_tool(w, call);
        if (!strcmp(call->name, "bash") ||
            !strcmp(call->name, "bash_status") ||
            !strcmp(call->name, "bash_stop"))
            agent_bash_publish_observation(w, remote);
        return remote;
    }

    if (!strcmp(call->name, "read")) return agent_tool_read(w, call);
    if (!strcmp(call->name, "more")) return agent_tool_more(w, call);
    if (!strcmp(call->name, "write")) return agent_tool_write(w, call);
    if (!strcmp(call->name, "list")) return agent_tool_list(call);
    if (!strcmp(call->name, "edit")) return agent_tool_edit(w, call);
    if (!strcmp(call->name, "search")) return agent_tool_search(w, call);
    if (!strcmp(call->name, "google_search")) return worker_tool_google_search(w, call);
    if (!strcmp(call->name, "visit_page")) return worker_tool_visit_page(w, call);

    if (!strcmp(call->name, "bash")) {
        const char *cmd = agent_tool_arg_value(call, "command");
        if (!cmd || !cmd[0]) return xstrdup("Tool error: bash requires command\n");
        int timeout = agent_parse_timeout(agent_tool_arg_value(call, "timeout_sec"));
        int refresh = agent_parse_int_default(agent_tool_arg_value(call, "refresh_sec"),
                                              60, 1, 3600);
        char err[160] = {0};
        agent_bash_job *job = agent_bash_start(w, cmd, timeout, err, sizeof(err));
        if (!job) {
            agent_buf_puts(&result, "Tool error: bash failed to start: ");
            agent_buf_puts(&result, err[0] ? err : "unknown error");
            agent_buf_puts(&result, "\n");
            return agent_buf_take(&result);
        }
        return agent_bash_job_tool_result(w, job, true, refresh, false, true);
    }

    if (!strcmp(call->name, "bash_status") ||
        !strcmp(call->name, "bash_stop"))
    {
        int job_id = agent_tool_job_id(call);
        pid_t pid = agent_tool_pid(call);
        agent_bash_job *job = agent_bash_find_job(w, job_id, pid);
        if (!job) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Tool error: bash job not found: job=%d pid=%ld\n",
                     job_id, (long)pid);
            return xstrdup(msg);
        }
        int refresh = agent_parse_int_default(agent_tool_arg_value(call, "refresh_sec"),
                                              60, 1, 3600);
        bool stop = !strcmp(call->name, "bash_stop");
        bool wait = stop;
        return agent_bash_job_tool_result(w, job, wait, refresh, stop, true);
    }

    {
        char header[256];
        snprintf(header, sizeof(header), "\n[tool:%s] unknown tool\n", call->name);
        agent_publish(w, header, strlen(header));
        agent_buf_puts(&result, "Tool error: unknown tool: ");
        agent_buf_puts(&result, call->name);
        agent_buf_puts(&result, "\n");
        return agent_buf_take(&result);
    }
}

#define AGENT_WEB_HEAD_BYTES (8 * 1024)
#define AGENT_WEB_HEAD_LINES 100

static int tool_count_lines(const char *text) {
    if (!text || !text[0]) return 0;
    int lines = 0;
    for (const char *cursor = text; *cursor; cursor++)
        if (*cursor == '\n') lines++;
    if (text[strlen(text) - 1] != '\n') lines++;
    return lines;
}

static char *tool_string_head(const char *text, int maximum_lines,
                              size_t maximum_bytes, int *lines_read,
                              bool *byte_limited) {
    if (lines_read) *lines_read = 0;
    if (byte_limited) *byte_limited = false;
    if (!text) return xstrdup("");
    size_t used = 0;
    int lines = 0;
    while (text[used] && used < maximum_bytes && lines < maximum_lines)
        if (text[used++] == '\n') lines++;
    if (text[used] && used >= maximum_bytes && byte_limited)
        *byte_limited = true;
    if (used && text[used - 1] != '\n' && lines < maximum_lines) lines++;
    if (lines_read) *lines_read = lines;
    return xstrndup(text, used);
}

static bool tool_write_temp_text(const char *prefix, const char *text,
                                 char *path, size_t path_length,
                                 char *error, size_t error_length) {
    char template_path[PATH_MAX];
    snprintf(template_path, sizeof(template_path), "/tmp/%s_XXXXXX", prefix);
    int fd = mkstemp(template_path);
    if (fd < 0) {
        snprintf(error, error_length, "failed to create temporary file: %s",
                 strerror(errno));
        return false;
    }
    const char *cursor = text ? text : "";
    size_t remaining = text ? strlen(text) : 0;
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            snprintf(error, error_length,
                     "failed to write temporary file: %s", strerror(errno));
            close(fd);
            unlink(template_path);
            return false;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    if (close(fd) != 0) {
        snprintf(error, error_length, "failed to close temporary file: %s",
                 strerror(errno));
        unlink(template_path);
        return false;
    }
    snprintf(path, path_length, "%s", template_path);
    return true;
}

static char *worker_tool_google_search(agent_worker *worker,
                                       const agent_tool_call *call) {
    const char *query = agent_tool_arg_value(call, "query");
    if (!query || !query[0])
        return xstrdup("Tool error: google_search requires query\n");
    char error[256] = {0};
    agent_publishf_system_status(worker, "Searching Google for %s...", query);
    char *markdown = sirio_tool_web_google_search(
        worker->web, query, error, sizeof(error));
    if (markdown) return markdown;
    agent_buf result = {0};
    agent_buf_puts(&result, "Tool error: google_search failed: ");
    agent_buf_puts(&result, error[0] ? error : "unknown error");
    agent_buf_puts(&result, "\n");
    return agent_buf_take(&result);
}

static char *worker_tool_visit_page(agent_worker *worker,
                                    const agent_tool_call *call) {
    const char *url = agent_tool_arg_value(call, "url");
    if (!url || !url[0])
        return xstrdup("Tool error: visit_page requires url\n");
    char error[256] = {0};
    agent_publishf_system_status(worker, "Opening page %s...", url);
    char *markdown = sirio_tool_web_visit_page(
        worker->web, url, error, sizeof(error));
    if (!markdown) {
        agent_buf result = {0};
        agent_buf_puts(&result, "Tool error: visit_page failed: ");
        agent_buf_puts(&result, error[0] ? error : "unknown error");
        agent_buf_puts(&result, "\n");
        return agent_buf_take(&result);
    }

    char path[PATH_MAX];
    if (!tool_write_temp_text("sirio_web", markdown, path, sizeof(path),
                              error, sizeof(error))) {
        free(markdown);
        agent_buf result = {0};
        agent_buf_puts(&result, "Tool error: visit_page failed: ");
        agent_buf_puts(&result,
                       error[0] ? error : "could not store rendered page");
        agent_buf_puts(&result, "\n");
        return agent_buf_take(&result);
    }

    int total_lines = tool_count_lines(markdown);
    int shown_lines = 0;
    bool byte_limited = false;
    char *head = tool_string_head(markdown, AGENT_WEB_HEAD_LINES,
                                  AGENT_WEB_HEAD_BYTES, &shown_lines,
                                  &byte_limited);
    bool truncated = byte_limited || shown_lines < total_lines;
    agent_buf result = {0};
    char line[PATH_MAX + 256];
    snprintf(line, sizeof(line),
             "visit_page url=%s\noutput_path=%s (%zu bytes, %d lines)\n",
             url, path, strlen(markdown), total_lines);
    agent_buf_puts(&result, line);
    if (truncated) {
        snprintf(line, sizeof(line), "<head -%d %s>\n",
                 AGENT_WEB_HEAD_LINES, path);
        agent_buf_puts(&result, line);
        agent_buf_puts(&result, head);
        if (head[0] && head[strlen(head) - 1] != '\n')
            agent_buf_puts(&result, "\n");
        agent_buf_puts(&result, "</head>\n");
        agent_buf_puts(
            &result,
            "Use read path=<output_path> start_line=<line> max_lines=<count> raw=true to inspect more rendered Markdown.\n");
    } else {
        agent_buf_puts(&result, "<markdown>\n");
        agent_buf_puts(&result, head);
        if (head[0] && head[strlen(head) - 1] != '\n')
            agent_buf_puts(&result, "\n");
        agent_buf_puts(&result, "</markdown>\n");
    }
    free(head);
    free(markdown);
    return agent_buf_take(&result);
}

static bool sirio_tool_supported(const char *name) {
    return name &&
        (!strcmp(name, "read") || !strcmp(name, "more") ||
         !strcmp(name, "write") || !strcmp(name, "edit") ||
         !strcmp(name, "list") || !strcmp(name, "search") ||
         !strcmp(name, "bash") || !strcmp(name, "bash_status") ||
         !strcmp(name, "bash_stop") ||
         !strcmp(name, "google_search") ||
         !strcmp(name, "visit_page"));
}

sirio_tool_runtime *sirio_tool_runtime_create(
        const sirio_tool_options *options, char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    const char *home = options && options->home_dir && options->home_dir[0] ?
                       options->home_dir : getenv("HOME");
    if (!home || !home[0]) home = ".";
    if (!agent_mkdir_p(home)) {
        if (error && error_len)
            snprintf(error, error_len, "cannot create HOME %s: %s",
                     home, strerror(errno));
        return NULL;
    }

    sirio_tool_runtime *runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        if (error && error_len)
            snprintf(error, error_len, "tool runtime allocation failed");
        return NULL;
    }
    runtime->config.gen.ctx_size =
        options && options->context_size > 0 ? options->context_size : 1000000;
    runtime->config.edit_upto = options && options->edit_upto;
    runtime->worker.cfg = &runtime->config;
    runtime->worker.next_bash_job_id = 1;
    runtime->worker.log = options ? options->log : NULL;
    runtime->worker.log_private_data =
        options ? options->log_private_data : NULL;
    if (pthread_mutex_init(&runtime->worker.mu, NULL) != 0) {
        if (error && error_len)
            snprintf(error, error_len,
                     "tool runtime mutex initialization failed");
        free(runtime);
        return NULL;
    }

    sirio_tool_web_options web_options = {
        .home_dir = home,
        .port = 9333,
        .confirm = options ? options->confirm : NULL,
        .confirm_private_data =
            options ? options->confirm_private_data : NULL,
        .log = options ? options->log : NULL,
        .log_private_data = options ? options->log_private_data : NULL,
    };
    runtime->worker.web = sirio_tool_web_create(&web_options);
    return runtime;
}

char *sirio_tool_runtime_execute(
        sirio_tool_runtime *runtime, const char *tool,
        const sirio_tool_argument *arguments, size_t argument_count) {
    if (!runtime)
        return xstrdup("Tool error: worker runtime is not initialized\n");
    if (!sirio_tool_supported(tool))
        return xstrdup("Tool error: tool is not handled by the container runner\n");
    if (argument_count && !arguments)
        return xstrdup("Tool error: missing tool arguments\n");
    if (argument_count > (size_t)INT_MAX)
        return xstrdup("Tool error: too many tool arguments\n");

    agent_tool_call call = {.name = xstrdup(tool)};
    for (size_t index = 0; index < argument_count; index++) {
        if (!arguments[index].name || !arguments[index].value) {
            agent_tool_call_free(&call);
            return xstrdup("Tool error: invalid tool argument\n");
        }
        agent_tool_call_add_arg(
            &call, arguments[index].name, arguments[index].value,
            strlen(arguments[index].value), arguments[index].is_string);
    }
    char *result = agent_execute_tool_call(&runtime->worker, &call);
    agent_tool_call_free(&call);
    return result ? result : xstrdup("Tool error: empty runner result\n");
}

void sirio_tool_runtime_destroy(sirio_tool_runtime *runtime) {
    if (!runtime) return;
    agent_bash_jobs_free(&runtime->worker);
    sirio_tool_web_free(runtime->worker.web);
    pthread_mutex_destroy(&runtime->worker.mu);
    free(runtime);
}

#ifndef SIRIO_TOOL_RUNNER_TEST
static int runner_web_confirm(void *private_data, const char *message,
                              char *error, size_t error_len) {
    (void)private_data;
    (void)message;
    (void)error;
    (void)error_len;
    return 1;
}

static void runner_web_log(void *private_data, const char *message) {
    (void)private_data;
    if (message && message[0])
        fprintf(stderr, "sirio-tool-runner: web: %s\n", message);
}

static bool runner_parse_context(const char *text, int *value) {
    if (!text || !text[0]) return false;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < 1 || parsed > INT_MAX)
        return false;
    *value = (int)parsed;
    return true;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    sirio_tool_options options = {
        .home_dir = getenv("HOME"),
        .confirm = runner_web_confirm,
        .log = runner_web_log,
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--edit-upto")) {
            options.edit_upto = true;
        } else if (!strcmp(argv[i], "--ctx") && i + 1 < argc) {
            if (!runner_parse_context(argv[++i], &options.context_size)) {
                fprintf(stderr, "sirio-tool-runner: invalid context size: %s\n",
                        argv[i]);
                return 2;
            }
        } else {
            fprintf(stderr, "sirio-tool-runner: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    char error[192] = {0};
    sirio_tool_runtime *runtime =
        sirio_tool_runtime_create(&options, error, sizeof(error));
    if (!runtime) {
        fprintf(stderr, "sirio-tool-runner: %s\n",
                error[0] ? error : "worker initialization failed");
        return 1;
    }

    fputs("READY\n", stdout);
    fflush(stdout);

    char *line = NULL;
    size_t line_cap = 0;
    int status = 0;
    while (getline(&line, &line_cap, stdin) >= 0) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' ||
                          line[length - 1] == '\r'))
            line[--length] = '\0';

        sirio_tool_request request;
        error[0] = '\0';
        if (sirio_tool_protocol_decode_request(
                line, &request, error, sizeof(error)) != 0) {
            char *response = sirio_tool_protocol_encode_response(
                error[0] ? error : "malformed request", true);
            if (!response || fprintf(stdout, "%s\n", response) < 0 ||
                fflush(stdout) != 0) {
                free(response);
                status = 1;
                break;
            }
            free(response);
            continue;
        }

        if (!strcmp(request.tool, "shutdown")) {
            char *response = sirio_tool_protocol_encode_response(
                "shutdown", false);
            int failed = !response || fprintf(stdout, "%s\n", response) < 0 ||
                         fflush(stdout) != 0;
            free(response);
            sirio_tool_request_free(&request);
            if (failed) status = 1;
            break;
        }

        char *result = sirio_tool_runtime_execute(
            runtime, request.tool, request.arguments, request.argument_count);
        char *response = sirio_tool_protocol_encode_response(
            result ? result : "Tool error: empty runner result\n", false);
        bool failed = !response || fprintf(stdout, "%s\n", response) < 0 ||
                      fflush(stdout) != 0;
        free(response);
        free(result);
        sirio_tool_request_free(&request);
        if (failed) {
            status = 1;
            break;
        }
    }

    free(line);
    sirio_tool_runtime_destroy(runtime);
    return status;
}
#endif
