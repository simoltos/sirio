/* sirio_help.c - Sirio command-line and interactive help renderer. */

#include "sirio_core.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *off;
    const char *cyan;
    const char *green;
    const char *red;
    const char *purple;
    const char *title;
    const char *grey;
    const char *white;
    const char *bright;
    bool enabled;
} sirio_help_colors;

static sirio_help_colors sirio_help_make_colors(FILE *fp) {
    sirio_help_colors colors = {0};
    if (!isatty(fileno(fp))) return colors;
    colors.off = "\x1b[0m";
    colors.cyan = "\x1b[38;5;81m";
    colors.green = "\x1b[38;5;114m";
    colors.red = "\x1b[38;5;203m";
    colors.purple = "\x1b[38;5;141m";
    colors.title = "\x1b[1;38;5;250m";
    colors.grey = "\x1b[38;5;240m";
    colors.white = "\x1b[38;5;252m";
    colors.bright = "\x1b[1;38;5;231m";
    colors.enabled = true;
    return colors;
}

static const char *sirio_help_description(const char *command) {
    if (!command)
        return "Run a terminal coding agent through a user-defined model "
               "interface, with local tools and delegated models.";
    if (!strcmp(command, "auth"))
        return "Manage provider authentication.";
    if (!strcmp(command, "catalog"))
        return "List supported providers and models.";
    if (!strcmp(command, "sessions"))
        return "Manage and resume saved sessions.";
    return "";
}

static void sirio_help_header(FILE *fp, const sirio_help_colors *colors,
                              const char *command) {
    fprintf(fp, "%ssirio%s%s%s\n", colors->bright ? colors->bright : "",
            command ? " " : "", command ? command : "",
            colors->off ? colors->off : "");
    fprintf(fp, "%s\n\n", sirio_help_description(command));
    if (command)
        fprintf(fp, "Usage: sirio %s [options]\n\n", command);
    else
        fputs("Usage: sirio [command] [options]\n\n", fp);
}

static void sirio_help_title(FILE *fp, const sirio_help_colors *colors,
                             const char *text) {
    fprintf(fp, "%s%s%s\n", colors->title ? colors->title : "", text,
            colors->off ? colors->off : "");
}

static const char *sirio_help_metavariable(const char *name) {
    if (name[0] != '-' && name[0] != '/') return NULL;
    for (const char *space = strchr(name, ' ');
         space;
         space = strchr(space + 1, ' ')) {
        if (!space[1]) continue;
        unsigned char first = (unsigned char)space[1];
        if ((first >= 'A' && first <= 'Z') || first == '[')
            return space;
    }
    return NULL;
}

static void sirio_help_item(FILE *fp, const sirio_help_colors *colors,
                            const char *item_color,
                            const char *name, const char *description) {
    const int column = 30;
    int width = (int)strlen(name);
    if (!colors->enabled) {
        if (width > column)
            fprintf(fp, "  %s\n      %s\n", name, description);
        else
            fprintf(fp, "  %-30s %s\n", name, description);
        return;
    }

    const char *meta = sirio_help_metavariable(name);
    fputs("  ", fp);
    if (meta) {
        fprintf(fp, "%s%.*s%s %s%s%s", item_color,
                (int)(meta - name), name, colors->off,
                colors->bright, meta + 1, colors->off);
    } else {
        fprintf(fp, "%s%s%s", item_color, name, colors->off);
    }
    if (width > column) {
        fprintf(fp, "\n      %s%s%s\n", colors->white, description,
                colors->off);
        return;
    }
    for (int i = width; i < column; i++) fputc(' ', fp);
    fprintf(fp, " %s|%s %s%s%s\n", colors->grey, colors->off,
            colors->white, description, colors->off);
}

static void sirio_help_option(FILE *fp, const sirio_help_colors *colors,
                              const char *name, const char *description) {
    sirio_help_item(fp, colors, colors->cyan, name, description);
}

static void sirio_help_command(FILE *fp, const sirio_help_colors *colors,
                               const char *color,
                               const char *name, const char *description) {
    sirio_help_item(fp, colors, color, name, description);
}

static void sirio_help_cli_commands(FILE *fp,
                                    const sirio_help_colors *colors) {
    sirio_help_title(fp, colors, "Commands");
    sirio_help_command(fp, colors, colors->green, "auth",
                       "Manage provider authentication.");
    sirio_help_command(fp, colors, colors->green, "catalog",
                       "List supported providers and models.");
    sirio_help_command(fp, colors, colors->green, "sessions",
                       "Manage and resume saved sessions.");
    fputc('\n', fp);
}

static void sirio_help_auth(FILE *fp, const sirio_help_colors *colors) {
    sirio_help_title(fp, colors, "Options");
    sirio_help_option(fp, colors, "--api-key PROVIDER",
                      "Prompt for and save that provider's API key.");
    sirio_help_option(fp, colors, "--login PROVIDER",
                      "Run that provider's interactive OAuth login.");
    sirio_help_option(fp, colors, "--logout PROVIDER",
                      "Remove that provider's saved credentials.");
    sirio_help_option(fp, colors, "--status",
                      "List credential availability without secrets.");
    sirio_help_option(fp, colors, "--stdin",
                      "With --api-key, read one secret line from stdin.");
    sirio_help_option(fp, colors, "-y, --yes",
                      "Skip confirmation for replacement or logout.");
    sirio_help_option(fp, colors, "-h, --help",
                      "Show help for the auth command.");
    fputc('\n', fp);
}

static void sirio_help_catalog(FILE *fp, const sirio_help_colors *colors) {
    sirio_help_title(fp, colors, "Options");
    sirio_help_option(fp, colors, "--providers",
                      "List supported providers and credential availability.");
    sirio_help_option(fp, colors, "--models",
                      "List supported models and their interface scope.");
    sirio_help_option(fp, colors, "--provider PROVIDER",
                      "With --models, restrict the list to one provider.");
    sirio_help_option(fp, colors, "--default MODEL[,MODEL...]",
                      "Add models to the main interface.");
    sirio_help_option(fp, colors, "--remove MODEL[,MODEL...]",
                      "Remove models from the main interface.");
    sirio_help_option(fp, colors, "-h, --help",
                      "Show help for the catalog command.");
    fputc('\n', fp);
}

static void sirio_help_sessions(FILE *fp,
                                const sirio_help_colors *colors) {
    sirio_help_title(fp, colors, "Options");
    sirio_help_option(fp, colors, "--list",
                      "List saved sessions by recent use, then exit.");
    sirio_help_option(fp, colors, "--resume ID",
                      "Resume an unambiguous saved-session SHA prefix.");
    sirio_help_option(fp, colors, "--turns N",
                      "With --resume, show the last N user turns (1-200; default 3).");
    sirio_help_option(fp, colors, "--delete ID",
                      "Confirm and delete a saved session.");
    sirio_help_option(fp, colors, "--strip ID",
                      "Validate and canonically rewrite a saved session.");
    sirio_help_option(fp, colors, "-y, --yes",
                      "Skip confirmation for --delete.");
    sirio_help_option(fp, colors, "-h, --help",
                      "Show help for the sessions command.");
    fputs("\nGeneral run options may be used with --resume, except "
          "-p/--prompt, --non-interactive, --raw-prompt, and "
          "-sys/--system.\n", fp);
    fputc('\n', fp);
}

static void sirio_help_general_options(FILE *fp,
                                       const sirio_help_colors *colors) {
    sirio_help_title(fp, colors, "Options");
    sirio_help_option(fp, colors, "-m, --model MODEL",
                      "Interface model for this invocation.");
    sirio_help_option(fp, colors, "-p, --prompt TEXT",
                      "Submit the initial prompt.");
    sirio_help_option(fp, colors, "--non-interactive",
                      "Run one prompt without the terminal UI; requires -p.");
    sirio_help_option(fp, colors, "--raw-prompt",
                      "Send a non-interactive prompt without system or tool context.");
    sirio_help_option(fp, colors, "--edit-upto",
                      "Allow explicit model-emitted [upto] edit markers.");
    sirio_help_option(fp, colors, "-sys, --system TEXT",
                      "Replace Sirio's extra identity text.");
    sirio_help_option(fp, colors, "-n, --tokens N",
                      "Maximum provider output tokens when supported. Default: provider limit");
    sirio_help_option(fp, colors, "--trace FILE",
                      "Append agent, provider, and tool diagnostics to FILE.");
    sirio_help_option(fp, colors, "-C, --chdir DIR",
                      "Change the workspace directory before startup.");
    sirio_help_option(fp, colors, "--temp F",
                      "Sampling temperature for providers that support it.");
    sirio_help_option(fp, colors, "--top-p F",
                      "Nucleus probability for providers that support it.");
    sirio_help_option(fp, colors, "--think LEVEL",
                      "Reasoning effort: none, low, medium, high, xhigh, or max.");
    sirio_help_option(fp, colors, "-h, --help",
                      "Show command-line and interactive help.");
    fputc('\n', fp);
}

static void sirio_help_commands(FILE *fp, const sirio_help_colors *colors,
                                bool include_controls) {
    sirio_help_title(fp, colors, "Runtime Commands");
    sirio_help_command(fp, colors, colors->red, "/help",
                       "Show interactive commands.");
    sirio_help_command(fp, colors, colors->red, "/save",
                      "Save the current transcript atomically.");
    sirio_help_command(fp, colors, colors->red, "/compact",
                      "Summarize durable state and retain a recent verbatim tail.");
    sirio_help_command(fp, colors, colors->red, "/list",
                      "List saved sessions by recent use.");
    sirio_help_command(fp, colors, colors->red, "/model MODEL [THINKING]",
                      "Change interface model while idle.");
    sirio_help_command(fp, colors, colors->red, "/switch ID",
                      "Load a saved session and show recent history.");
    sirio_help_command(fp, colors, colors->red, "/del ID",
                       "Delete a saved session.");
    sirio_help_command(fp, colors, colors->red, "/strip ID",
                      "Canonically rewrite a saved session.");
    sirio_help_command(fp, colors, colors->red, "/history [N]",
                      "Show N recent user turns.");
    sirio_help_command(fp, colors, colors->red, "/new",
                       "Start a fresh session.");
    sirio_help_command(fp, colors, colors->red, "/quit, /exit", "Exit.");
    fputc('\n', fp);
    if (!include_controls) return;

    sirio_help_title(fp, colors, "Interactive Controls");
    sirio_help_command(fp, colors, colors->purple, "Ctrl+C",
                       "Interrupt generation; while idle, clear edited text.");
    sirio_help_command(fp, colors, colors->purple, "Enter",
                       "Queue text while the agent is busy.");
    sirio_help_command(fp, colors, colors->purple, "Ctrl+X",
                       "Edit the first queued prompt.");
    sirio_help_command(fp, colors, colors->purple, "ESC",
                       "Interrupt and send the queued prompt immediately.");
    sirio_help_command(fp, colors, colors->purple, "Ctrl+D",
                       "Exit from an empty prompt.");
    sirio_help_command(fp, colors, colors->purple, "Alt+, / Alt+.",
                       "Decrease or increase reasoning effort while idle.");
    sirio_help_command(fp, colors, colors->purple, "Alt+k / Alt+l",
                       "Select the previous or next interface model.");
    fputc('\n', fp);
}

static void sirio_help_default(FILE *fp, const sirio_help_colors *colors) {
    sirio_help_cli_commands(fp, colors);
    sirio_help_general_options(fp, colors);
    sirio_help_commands(fp, colors, true);
}

int sirio_help_print(FILE *fp, const char *topic) {
    sirio_help_colors colors = sirio_help_make_colors(fp);
    bool known = !topic ||
                 !strcmp(topic, "auth") ||
                 !strcmp(topic, "catalog") ||
                 !strcmp(topic, "sessions") ||
                 !strcmp(topic, "commands-internal");
    if (!known) {
        fprintf(fp, "sirio: unknown help topic '%s'\n\n", topic);
        topic = NULL;
    }

    const char *command = known && topic &&
                          strcmp(topic, "commands-internal") ? topic : NULL;
    sirio_help_header(fp, &colors, command);
    if (!topic)
        sirio_help_default(fp, &colors);
    else if (!strcmp(topic, "auth"))
        sirio_help_auth(fp, &colors);
    else if (!strcmp(topic, "catalog"))
        sirio_help_catalog(fp, &colors);
    else if (!strcmp(topic, "sessions"))
        sirio_help_sessions(fp, &colors);
    else if (!strcmp(topic, "commands-internal"))
        sirio_help_commands(fp, &colors, true);
    else
        sirio_help_default(fp, &colors);
    return known ? 0 : 2;
}
