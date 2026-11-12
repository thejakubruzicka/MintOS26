/* 26.5.014.0: Terminal runs programs stored on MintOS HD.
 *
 * A command whose first word is a path ("./prog", "/System/Programs/lua")
 * or a name found in PATH (/System/Programs, then ~/Programs) is spawned as
 * its own process with a stdin pipe and one stdout/stderr pipe that behave
 * like a terminal (line-buffered output, isatty() true). Terminal reads the
 * output every frame, shows the unfinished last line (prompts) with the
 * user's typing after it, sends each typed line to the program's stdin, and
 * Ctrl+C kills it (SIGINT), Ctrl+D ends its input. Several programs may be
 * alive at once (e.g. started with a trailing '&'); the foreground one owns
 * Terminal's input. */
#include "mintde.h"
#include "procterm.h"
#include <mintos/proc.h>

#define PT_COLS 92u
#define PT_ARGS 32u
#define PT_BG_MAX 4u
#define MINT_PROC_MAX_LIST 24u

typedef struct pt_job {
    int64_t pid;
    int32_t in_fd, out_fd;
    char name[32];
    char partial[PT_COLS + 1u];
    size_t partial_len;
    int background;
} PT_JOB;

static PT_JOB fg = {.pid = 0, .in_fd = -1, .out_fd = -1};
static PT_JOB bg[PT_BG_MAX];
static char pt_line[PT_COLS + 1u];
static uint8_t pt_args[MINT_PROC_ARGS_MAX];
static char pt_path[MINT_PROC_PATH_MAX], pt_word[PT_ARGS][128];

static int same(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) ++i;
    return a[i] == b[i];
}

/* Split a command line into words; "..." and '...' group, \x escapes. */
static int split_words(const char *cmd, int *background) {
    int n = 0;
    size_t i = 0;
    *background = 0;
    while (cmd[i] && n < (int)PT_ARGS) {
        while (cmd[i] == ' ' || cmd[i] == '\t') ++i;
        if (!cmd[i]) break;
        size_t o = 0;
        char quote = 0;
        int had = 0;
        while (cmd[i] && (quote || (cmd[i] != ' ' && cmd[i] != '\t'))) {
            char c = cmd[i++];
            if (!quote && (c == '"' || c == '\'')) { quote = c; had = 1; continue; }
            if (quote && c == quote) { quote = 0; continue; }
            if (c == '\\' && cmd[i] && quote != '\'') c = cmd[i++];
            if (o + 1u < sizeof(pt_word[0])) pt_word[n][o++] = c;
        }
        pt_word[n][o] = '\0';
        if (o || had) ++n;
    }
    if (n && same(pt_word[n - 1], "&")) { *background = 1; --n; }
    return n;
}

static void join(char *out, size_t cap, const char *dir, const char *leaf) {
    copy_text(out, cap, dir);
    if (!(dir[0] == '/' && !dir[1])) str_append(out, cap, "/");
    str_append(out, cap, leaf);
}

/* Normalise "/a/./b/../c" in place. */
static void normalise(char *path) {
    char tmp[MINT_PROC_PATH_MAX];
    size_t n = 0, i = 0;
    tmp[n++] = '/';
    while (path[i]) {
        while (path[i] == '/') ++i;
        if (!path[i]) break;
        size_t s = i;
        while (path[i] && path[i] != '/') ++i;
        size_t len = i - s;
        if (len == 1 && path[s] == '.') continue;
        if (len == 2 && path[s] == '.' && path[s + 1] == '.') {
            while (n > 1 && tmp[n - 1] != '/') --n;
            if (n > 1) --n;
            continue;
        }
        if (n > 1 && n + 1 < sizeof(tmp)) tmp[n++] = '/';
        for (size_t k = 0; k < len && n + 1 < sizeof(tmp); ++k) tmp[n++] = path[s + k];
    }
    tmp[n] = '\0';
    copy_text(path, MINT_PROC_PATH_MAX, tmp);
}

static int is_file(const char *path) {
    MINT_MFS_STAT st;
    return mint_path_stat(path, &st) == 0 && st.type == MINT_MFS_FILE;
}

static int resolve_program(const char *word, const char *cwd, const char *home) {
    int has_slash = 0;
    for (size_t i = 0; word[i]; ++i) if (word[i] == '/') has_slash = 1;
    if (has_slash) {
        if (word[0] == '/') copy_text(pt_path, sizeof(pt_path), word);
        else if (word[0] == '~' && word[1] == '/') join(pt_path, sizeof(pt_path), home, word + 2);
        else join(pt_path, sizeof(pt_path), cwd, word);
        normalise(pt_path);
        return 1;   /* explicit path: report errors for it */
    }
    const char *dirs[2] = {"/System/Programs", 0};
    char user_dir[MINT_PROC_PATH_MAX];
    join(user_dir, sizeof(user_dir), home, "Programs");
    dirs[1] = user_dir;
    for (int d = 0; d < 2; ++d) {
        join(pt_path, sizeof(pt_path), dirs[d], word);
        if (is_file(pt_path)) return 1;
    }
    return 0;
}

static const char *spawn_error(int64_t rc) {
    switch (-rc) {
    case MINT_ENOENT: return "no such file";
    case MINT_ENOEXEC: return "not a MintOS program (no ELF header)";
    case MINT_EACCES: return "is a folder, not a program";
    case MINT_EAGAIN: return "too many programs are running";
    case MINT_ENOMEM: return "not enough memory";
    case MINT_ENODEV: return "no MintOS HD is mounted";
    case MINT_EMFILE: return "too many open pipes";
    default: return "could not start";
    }
}

static void args_add(uint32_t *len, const char *s) {
    size_t n = text_length(s);
    if (*len + n + 1u > sizeof(pt_args)) return;
    for (size_t i = 0; i < n; ++i) pt_args[*len + i] = (uint8_t)s[i];
    pt_args[*len + n] = 0;
    *len += (uint32_t)n + 1u;
}

static void clear_job(PT_JOB *j) {
    if (j->in_fd >= 0) (void)mint_fd_close(j->in_fd);
    if (j->out_fd >= 0) (void)mint_fd_close(j->out_fd);
    j->pid = 0; j->in_fd = -1; j->out_fd = -1; j->partial_len = 0; j->partial[0] = '\0';
    j->background = 0;
}

static void list_processes(procterm_push_fn push) {
    static MINT_PROC_INFO list[MINT_PROC_MAX_LIST];
    int64_t n = mint_proc_list(list, MINT_PROC_MAX_LIST);
    if (n < 0) { push("ps: process list unavailable"); return; }
    push("  PID  PPID  STATE     MEMORY     PEAK    CPU  NAME");
    for (int64_t i = 0; i < n; ++i) {
        char row[PT_COLS + 1u], num[24];
        row[0] = '\0';
        format_u64(num, sizeof(num), list[i].pid);
        for (size_t p = text_length(num); p < 5u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), num);
        format_u64(num, sizeof(num), list[i].parent_pid);
        for (size_t p = text_length(num); p < 6u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), num);
        str_append(row, sizeof(row), list[i].state == MINT_PROC_INFO_ZOMBIE ? "  exited  " :
                                     list[i].state == MINT_PROC_INFO_STARTING ? "  starting" : "  running ");
        format_bytes(num, sizeof(num), list[i].owned_pages * 4096u);
        for (size_t p = text_length(num); p < 10u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), num);
        format_bytes(num, sizeof(num), list[i].peak_pages * 4096u);
        for (size_t p = text_length(num); p < 9u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), num);
        format_u64(num, sizeof(num), list[i].cpu_ticks / 100u);
        str_append(num, sizeof(num), "s");
        for (size_t p = text_length(num); p < 7u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), num);
        str_append(row, sizeof(row), "  ");
        str_append(row, sizeof(row), list[i].name);
        push(row);
    }
}

int procterm_command(const char *cmd, const char *cwd, const char *home, procterm_push_fn push) {
    int background = 0;
    int argc = split_words(cmd, &background);
    if (!argc) return 0;
    if (same(pt_word[0], "ps")) { list_processes(push); return 1; }
    if (same(pt_word[0], "kill")) {
        if (argc < 2) { push("usage: kill <pid>"); return 1; }
        uint64_t pid = 0;
        for (const char *c = pt_word[1]; *c >= '0' && *c <= '9'; ++c) pid = pid * 10u + (uint64_t)(*c - '0');
        int64_t rc = mint_proc_kill(pid, MINT_SIGTERM);
        push(rc == 0 ? "kill: signal sent" : rc == -MINT_EPERM ? "kill: not a program started from this desktop"
                                                               : "kill: no such process");
        return 1;
    }
    if (same(pt_word[0], "install-programs")) {
        int64_t rc = mint_programs_install();
        char row[64] = "install-programs: ";
        if (rc < 0) str_append(row, sizeof(row), "no MintOS HD is mounted");
        else { char num[16]; format_u64(num, sizeof(num), (uint64_t)rc); str_append(row, sizeof(row), num);
               str_append(row, sizeof(row), " files written to /System/Programs and /Library/Examples"); }
        push(row);
        return 1;
    }
    if (same(pt_word[0], "which")) {
        if (argc < 2) { push("usage: which <program>"); return 1; }
        push(resolve_program(pt_word[1], cwd, home) && is_file(pt_path) ? pt_path : "which: not found in PATH");
        return 1;
    }
    if (!resolve_program(pt_word[0], cwd, home)) return 0;
    if (fg.pid) { push("Terminal is busy: wait for the running program or press Ctrl+C"); return 1; }
    uint32_t len = 0;
    for (int i = 0; i < argc; ++i) args_add(&len, pt_word[i]);
    char env[MINT_PROC_PATH_MAX + 8];
    args_add(&len, "PATH=/System/Programs");
    copy_text(env, sizeof(env), "HOME="); str_append(env, sizeof(env), home); args_add(&len, env);
    copy_text(env, sizeof(env), "PWD="); str_append(env, sizeof(env), cwd); args_add(&len, env);
    args_add(&len, "TERM=dumb");
    args_add(&len, "USER=mintos");
    args_add(&len, "LUA_PATH=/Library/Lua/?.lua;./?.lua");
    MINT_SPAWN_REQUEST req;
    for (size_t i = 0; i < sizeof(req); ++i) ((uint8_t *)&req)[i] = 0;
    req.version = MINT_SPAWN_VERSION;
    req.flags = MINT_SPAWN_PIPE_STDIO | MINT_SPAWN_TTY;
    req.path = (uint64_t)(uintptr_t)pt_path;
    req.cwd = (uint64_t)(uintptr_t)cwd;
    req.args = (uint64_t)(uintptr_t)pt_args;
    req.args_bytes = len;
    req.argc = (uint32_t)argc;
    req.envc = 6u;
    int64_t pid = mint_proc_spawn(&req);
    if (pid < 0) {
        char row[PT_COLS + 1u];
        copy_text(row, sizeof(row), pt_word[0]); str_append(row, sizeof(row), ": ");
        str_append(row, sizeof(row), spawn_error(pid));
        push(row);
        return 1;
    }
    PT_JOB *j = &fg;
    if (background) {
        j = 0;
        for (uint32_t i = 0; i < PT_BG_MAX; ++i) if (!bg[i].pid) { j = &bg[i]; break; }
        if (!j) {
            push("too many background programs; killing the new one");
            (void)mint_proc_kill((uint64_t)pid, MINT_SIGKILL);
            if (req.stdin_fd >= 0) (void)mint_fd_close(req.stdin_fd);
            if (req.stdout_fd >= 0) (void)mint_fd_close(req.stdout_fd);
            return 1;
        }
    }
    j->pid = pid; j->in_fd = req.stdin_fd; j->out_fd = req.stdout_fd;
    j->partial_len = 0; j->partial[0] = '\0'; j->background = background;
    copy_text(j->name, sizeof(j->name), pt_word[0]);
    if (background) {
        char row[64] = "[started in background] pid ", num[16];
        format_u64(num, sizeof(num), (uint64_t)pid); str_append(row, sizeof(row), num);
        push(row);
        /* A background program has no keyboard: its stdin ends immediately. */
        if (j->in_fd >= 0) { (void)mint_fd_close(j->in_fd); j->in_fd = -1; }
    }
    return 1;
}

int procterm_active(void) { return fg.pid != 0; }
const char *procterm_partial(void) { return fg.partial; }
const char *procterm_hint(void) { return "Running\xE2\x80\xA6 (Ctrl+C to stop, Ctrl+D ends input)"; }

static void flush_partial(PT_JOB *j, procterm_push_fn push) {
    if (!j->partial_len) return;
    if (j->background) {
        copy_text(pt_line, sizeof(pt_line), "[");
        str_append(pt_line, sizeof(pt_line), j->name);
        str_append(pt_line, sizeof(pt_line), "] ");
        str_append(pt_line, sizeof(pt_line), j->partial);
        push(pt_line);
    } else push(j->partial);
    j->partial_len = 0;
    j->partial[0] = '\0';
}

static int pump_output(PT_JOB *j, procterm_push_fn push) {
    static uint8_t buf[4096];
    int changed = 0;
    for (int round = 0; round < 8 && j->out_fd >= 0; ++round) {
        int64_t n = mint_fd_read(j->out_fd, buf, sizeof(buf), MINT_IO_NONBLOCK);
        if (n <= 0) {
            if (n == 0) { (void)mint_fd_close(j->out_fd); j->out_fd = -1; }
            break;
        }
        changed = 1;
        for (int64_t i = 0; i < n; ++i) {
            uint8_t c = buf[i];
            if (c == '\n') { if (j->partial_len) flush_partial(j, push); else if (!j->background) push(""); continue; }
            if (c == '\r') continue;
            if (c == '\t') {
                do { j->partial[j->partial_len++] = ' '; } while ((j->partial_len % 4u) && j->partial_len < PT_COLS);
            } else if (c >= 32u && c != 127u) {
                j->partial[j->partial_len++] = (char)c;
            }
            j->partial[j->partial_len] = '\0';
            if (j->partial_len >= PT_COLS) flush_partial(j, push);
        }
    }
    return changed;
}

static void report_exit(PT_JOB *j, const MINT_PROC_STATUS *st, procterm_push_fn push) {
    char row[PT_COLS + 1u], num[24];
    row[0] = '\0';
    if (j->background) { copy_text(row, sizeof(row), "["); str_append(row, sizeof(row), j->name); str_append(row, sizeof(row), "] "); }
    if (st->reason == MINT_PROC_EXIT_NORMAL) {
        if (!st->code && !j->background) return;
        str_append(row, sizeof(row), "exited with status ");
        format_u64(num, sizeof(num), (uint64_t)st->code);
        str_append(row, sizeof(row), num);
    } else if (st->reason == MINT_PROC_EXIT_KILLED) {
        str_append(row, sizeof(row), st->code == (int32_t)MINT_SIGINT ? "interrupted (Ctrl+C)" : "terminated by signal ");
        if (st->code != (int32_t)MINT_SIGINT) { format_u64(num, sizeof(num), (uint64_t)st->code); str_append(row, sizeof(row), num); }
    } else if (st->reason == MINT_PROC_EXIT_FAULT) {
        str_append(row, sizeof(row), "crashed: CPU exception ");
        format_u64(num, sizeof(num), (uint64_t)st->code);
        str_append(row, sizeof(row), num);
        str_append(row, sizeof(row), st->code == 14 ? " (page fault)" : st->code == 13 ? " (protection fault)" : "");
    } else {
        str_append(row, sizeof(row), "could not be loaded (status 127)");
    }
    push(row);
}

static int step_job(PT_JOB *j, procterm_push_fn push) {
    if (!j->pid) return 0;
    int changed = pump_output(j, push);
    MINT_PROC_STATUS st;
    int64_t rc = mint_proc_wait(j->pid, &st, MINT_WAIT_NOHANG);
    if (rc == j->pid || rc == -MINT_ECHILD) {
        while (pump_output(j, push)) {}
        flush_partial(j, push);
        if (rc == j->pid) report_exit(j, &st, push);
        clear_job(j);
        changed = 1;
    }
    return changed;
}

int procterm_step(procterm_push_fn push) {
    int changed = step_job(&fg, push);
    for (uint32_t i = 0; i < PT_BG_MAX; ++i) changed |= step_job(&bg[i], push);
    return changed;
}

void procterm_submit(const char *line, procterm_push_fn push) {
    if (!fg.pid) return;
    copy_text(pt_line, sizeof(pt_line), fg.partial);
    str_append(pt_line, sizeof(pt_line), line);
    push(pt_line);                 /* echo, like a terminal in canonical mode */
    fg.partial_len = 0; fg.partial[0] = '\0';
    if (fg.in_fd < 0) return;
    size_t n = text_length(line);
    if (n) (void)mint_fd_write(fg.in_fd, line, n, MINT_IO_NONBLOCK);
    (void)mint_fd_write(fg.in_fd, "\n", 1u, MINT_IO_NONBLOCK);
}

void procterm_interrupt(procterm_push_fn push) {
    if (!fg.pid) return;
    flush_partial(&fg, push);
    push("^C");
    (void)mint_proc_kill((uint64_t)fg.pid, MINT_SIGINT);
}

void procterm_end_input(procterm_push_fn push) {
    (void)push;
    if (fg.pid && fg.in_fd >= 0) { (void)mint_fd_close(fg.in_fd); fg.in_fd = -1; }
}
