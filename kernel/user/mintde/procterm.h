#ifndef MINTDE_PROCTERM_H
#define MINTDE_PROCTERM_H
/* 26.5.014.0: Terminal runs programs from MintOS HD (see procterm.c). */
#include <stdint.h>
typedef void (*procterm_push_fn)(const char *line);
/* Built-ins (ps, kill, which, programs, install-programs) or a program found
 * through PATH / a path. Returns 1 if handled, 0 to let Terminal continue. */
int procterm_command(const char *cmd, const char *cwd, const char *home, procterm_push_fn push);
int procterm_active(void);
/* Pulls program output into Terminal; returns 1 if anything changed. */
int procterm_step(procterm_push_fn push);
/* The program's current unfinished output line (e.g. a "> " prompt). */
const char *procterm_partial(void);
const char *procterm_hint(void);
/* Typed line -> program's stdin (echoed after the partial line). */
void procterm_submit(const char *line, procterm_push_fn push);
void procterm_interrupt(procterm_push_fn push);   /* Ctrl+C */
void procterm_end_input(procterm_push_fn push);   /* Ctrl+D */
#endif
