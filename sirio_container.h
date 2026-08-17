#ifndef SIRIO_CONTAINER_H
#define SIRIO_CONTAINER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct sirio_container sirio_container;

typedef struct {
    char *name;
    char *value;
    bool is_string;
} sirio_container_argument;

typedef struct {
    const char *workspace;
    int context_size;
    bool edit_upto;
} sirio_container_options;

sirio_container *sirio_container_start(
    const sirio_container_options *options,
    char *error, size_t error_len);
int sirio_container_call(
    sirio_container *container, const char *tool,
    const sirio_container_argument *arguments, size_t argument_count,
    char **result, char *error, size_t error_len);
void sirio_container_stop(sirio_container *container);

#endif
