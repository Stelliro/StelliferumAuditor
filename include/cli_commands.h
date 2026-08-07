#ifndef CLI_COMMANDS_H
#define CLI_COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if argv[1] is a simple verb (help/list/pull/pipeline/push/restore/run). */
int cli_is_simple_command(int argc, char **argv);

/*
 * Run simple command surface.
 * Uses config/server_paths.ini + optional config/commands.ini recipes.
 * Returns process exit code (0 = success).
 */
int cli_run_simple_command(int argc, char **argv);

/* Print short help for simple verbs + recipe names. */
void cli_print_simple_help(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_COMMANDS_H */
