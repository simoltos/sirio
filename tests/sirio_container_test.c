#include "../sirio_container.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static int write_fake_podman(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    static const char script[] =
        "#!/bin/sh\n"
        "case \"$1 $2 $3\" in\n"
        "  'image exists cma') test \"${SIRIO_FAKE_IMAGE_MISSING:-0}\" != 1; exit $?;;\n"
        "  'container exists '*) exit 1;;\n"
        "  'run --rm -d') exit 0;;\n"
        "  'stop -t 0') printf 'stop\\n' >> \"$SIRIO_FAKE_LOG\"; exit 0;;\n"
        "esac\n"
        "if test \"$1\" = exec; then\n"
        "  printf 'READY\\n'\n"
        "  while IFS= read -r request; do\n"
        "    case \"$request\" in\n"
        "      *'\"tool\":\"shutdown\"'*) printf '{\"result\":\"shutdown\"}\\n'; exit 0;;\n"
        "      *) printf '{\"result\":\"fake result\"}\\n';;\n"
        "    esac\n"
        "  done\n"
        "fi\n"
        "exit 2\n";
    int ok = fwrite(script, 1, sizeof(script) - 1, file) ==
             sizeof(script) - 1 && fclose(file) == 0 &&
             chmod(path, 0700) == 0;
    return ok ? 0 : -1;
}

int main(void) {
    char directory[] = "/tmp/sirio-container-test-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char command[512], log_path[512];
    snprintf(command, sizeof(command), "%s/podman", directory);
    snprintf(log_path, sizeof(log_path), "%s/log", directory);
    CHECK(write_fake_podman(command) == 0);

    const char *old_path = getenv("PATH");
    char *saved_path = old_path ? strdup(old_path) : NULL;
    CHECK(setenv("PATH", directory, 1) == 0);
    CHECK(setenv("SIRIO_FAKE_LOG", log_path, 1) == 0);

    char error[256] = {0};
    sirio_container_options options = {
        .workspace = ".",
        .context_size = 1234,
        .edit_upto = true,
    };
    sirio_container *container = sirio_container_start(
        &options, error, sizeof(error));
    CHECK(container != NULL);
    if (container) {
        sirio_container_argument argument = {
            .name = "path", .value = "a file.c", .is_string = true,
        };
        char *result = NULL;
        CHECK(sirio_container_call(container, "read", &argument, 1,
                                   &result, error, sizeof(error)) == 0);
        CHECK(result && strcmp(result, "fake result") == 0);
        free(result);
        sirio_container_stop(container);
    }

    FILE *log = fopen(log_path, "rb");
    CHECK(log != NULL);
    if (log) {
        char line[32] = {0};
        CHECK(fgets(line, sizeof(line), log) != NULL);
        CHECK(strcmp(line, "stop\n") == 0);
        fclose(log);
    }

    CHECK(setenv("SIRIO_FAKE_IMAGE_MISSING", "1", 1) == 0);
    error[0] = '\0';
    container = sirio_container_start(&options, error, sizeof(error));
    CHECK(container == NULL);
    CHECK(strstr(error, "image 'cma' does not exist") != NULL);
    unsetenv("SIRIO_FAKE_IMAGE_MISSING");

    if (saved_path) CHECK(setenv("PATH", saved_path, 1) == 0);
    else CHECK(unsetenv("PATH") == 0);
    free(saved_path);
    unsetenv("SIRIO_FAKE_LOG");
    CHECK(unlink(command) == 0);
    CHECK(unlink(log_path) == 0);
    CHECK(rmdir(directory) == 0);

    if (failures) return 1;
    puts("sirio container tests: ok");
    return 0;
}
