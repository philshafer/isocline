/*
 * jig.c -- pty-driven and direct-call test harness for the isocline fork.
 *
 * See build/plans/isocline.rst ("Testing" section, Layer 2) for the full
 * design this implements:
 *
 *   - built the same way isocline.c builds itself: by #include-ing the
 *     amalgamated sources, so this file can reach isocline's private
 *     (static) functions as well as its public API;
 *   - ".keys" scripts drive a real pty running ic_readline() through a
 *     fixed 24x80 terminal, exercising isocline's raw-mode behavior;
 *   - ".direct" tests call straight into specific private functions to
 *     demonstrate bugs that don't need a terminal at all (bbcode/attr
 *     buffer handling);
 *   - output is rendered byte-for-byte: printable bytes (isprint())
 *     pass through unchanged, every other byte becomes "[%02x]\n".
 *
 * This file never writes to check/saved/ -- it only ever writes to
 * stdout, which the Makefile redirects into check/build/out/.
 */
#define _DARWIN_C_SOURCE 1

/*
 * isocline.c pulls in its own amalgamated sources, which trip two
 * warning classes under our strict flags: -Wunused-function (this jig
 * only calls a fraction of isocline's private functions) and
 * -Wformat-nonliteral (isocline's own debug_msg/bbcode_invalid/etc.
 * wrappers pass through a caller-supplied fmt by design).  Neither is
 * a real defect in isocline, so silence just these two around the
 * vendored include; jig.c's own code below stays fully warned.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#include "../src/isocline.c"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <util.h> /* openpty, login_tty (BSD/macOS) */

/*
 * Output captured by the pty drain thread.  Rendered at the very end
 * using the "[%02x]\n" rule for every non-printable byte.
 */
typedef struct {
    unsigned char *ca_buf;
    size_t ca_len;
    size_t ca_cap;
    pthread_mutex_t ca_lock;
} capture_t;

static void
capture_init (capture_t *cap)
{
    cap->ca_buf = NULL;
    cap->ca_len = 0;
    cap->ca_cap = 0;
    pthread_mutex_init(&cap->ca_lock, NULL);
}

static void
capture_append (capture_t *cap, const unsigned char *data, size_t n)
{
    pthread_mutex_lock(&cap->ca_lock);

    if (cap->ca_len + n > cap->ca_cap) {
        size_t newcap = (cap->ca_cap == 0 ? 4096 : cap->ca_cap);

        while (newcap < cap->ca_len + n)
            newcap *= 2;

        unsigned char *nbuf = (unsigned char*) realloc(cap->ca_buf, newcap);

        if (nbuf != NULL) {
            cap->ca_buf = nbuf;
            cap->ca_cap = newcap;
        }
    }

    if (cap->ca_buf != NULL) {
        memcpy(cap->ca_buf + cap->ca_len, data, n);
        cap->ca_len += n;
    }

    pthread_mutex_unlock(&cap->ca_lock);
}

static void
capture_write_escaped (const capture_t *cap, FILE *out)
{
    for (size_t i = 0; i < cap->ca_len; i++) {
        unsigned char b = cap->ca_buf[i];

        if (isprint(b))
            fputc(b, out);
        else
            fprintf(out, "[%02x]\n", (unsigned int) b);
    }
}

/*
 * Background thread that continuously reads the pty master, so no
 * output is lost while the main thread sleeps out an "i"/"w" delay.
 * This is deliberately not a strict ping-pong protocol.
 */
typedef struct {
    int da_fd;
    capture_t *da_cap;
} drain_arg_t;

static void *
drain_thread_main (void *argp)
{
    drain_arg_t *dap = (drain_arg_t *) argp;
    unsigned char tmp[4096];

    for (;;) {
        ssize_t n = read(dap->da_fd, tmp, sizeof(tmp));

        if (n > 0) {
            capture_append(dap->da_cap, tmp, (size_t) n);
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            usleep(1000);
            continue;
        }

        break; /* EOF (master closed / child gone) or a hard read error */
    }

    return NULL;
}

/*
 * Key encoding: turns one ".keys" token into the raw bytes a real
 * terminal would send, per the grammar confirmed in the plan:
 *
 *   - named keys are matched case-insensitively;
 *   - "C-<letter>" is case-insensitive (C-g == C-G == 0x07);
 *   - "E-<char>" is case-sensitive (E-k != E-K);
 *   - "u<hex>" sends a Unicode codepoint as its UTF-8 encoding.
 */
static size_t
utf8_encode (unsigned long cp, unsigned char *out)
{
    if (cp <= 0x7F) {
        out[0] = (unsigned char) cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (unsigned char) (0xC0 | (cp >> 6));
        out[1] = (unsigned char) (0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (unsigned char) (0xE0 | (cp >> 12));
        out[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char) (0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (unsigned char) (0xF0 | (cp >> 18));
        out[1] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
        out[2] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
        out[3] = (unsigned char) (0x80 | (cp & 0x3F));
        return 4;
    }

    return 0;
}

typedef struct {
    const char *na_name;
    const char *na_bytes;
    size_t na_len;
} named_key_t;

static const named_key_t named_keys[] = {
    { "SPACE", "\x20", 1 },
    { "ENTER", "\x0D", 1 },
    { "TAB", "\x09", 1 },
    { "BACKSPACE", "\x7F", 1 },
    { "UP", "\x1B[A", 3 },
    { "DOWN", "\x1B[B", 3 },
    { "LEFT", "\x1B[D", 3 },
    { "RIGHT", "\x1B[C", 3 },
    { "SHIFT-TAB", "\x1B[Z", 3 },
    { "C-UP", "\x1B[1;5A", 6 },
    { "C-DOWN", "\x1B[1;5B", 6 },
};

#define N_NAMED_KEYS (sizeof(named_keys) / sizeof(named_keys[0]))

/* out must have room for at least 8 bytes */
static bool
encode_key (const char *spec, unsigned char *out, size_t *outlen)
{
    size_t n = strlen(spec);

    if (n == 0)
        return false;

    for (size_t i = 0; i < N_NAMED_KEYS; i++) {
        if (strcasecmp(spec, named_keys[i].na_name) == 0) {
            memcpy(out, named_keys[i].na_bytes, named_keys[i].na_len);
            *outlen = named_keys[i].na_len;
            return true;
        }
    }

    if (n == 3 && (spec[0] == 'C' || spec[0] == 'c') && spec[1] == '-') {
        char ch = (char) toupper((unsigned char) spec[2]);

        if (ch < 'A' || ch > 'Z')
            return false;

        out[0] = (unsigned char) (ch - 'A' + 1);
        *outlen = 1;
        return true;
    }

    if (n == 3 && (spec[0] == 'E' || spec[0] == 'e') && spec[1] == '-') {
        out[0] = 0x1B;
        out[1] = (unsigned char) spec[2]; /* case preserved: E-k != E-K */
        *outlen = 2;
        return true;
    }

    if ((spec[0] == 'u' || spec[0] == 'U') && n > 1) {
        char *end = NULL;
        unsigned long cp = strtoul(spec + 1, &end, 16);

        if (end == spec + 1 || *end != 0)
            return false;

        size_t elen = utf8_encode(cp, out);

        if (elen == 0)
            return false;

        *outlen = elen;
        return true;
    }

    return false;
}

/*
 * The fake completer used by every ".keys" test: a handful of fixed
 * "show ..." commands, matched against the whole current line (these
 * test scripts never need isocline's own word-splitting).
 *
 * This is registered directly as the top-level completer (never
 * wrapped by ic_complete_word()), so unlike a narrowed-word completer
 * we have to tell isocline ourselves how much of the already-typed
 * line a completion replaces: all of "prefix", via
 * ic_add_completion_prim()'s delete_before.  ic_add_completions()
 * always passes delete_before=0 (meant for an already-narrowed
 * word), which here just inserts the full candidate at the cursor on
 * top of what was typed instead of replacing it.
 */
static void
jig_completer (ic_completion_env_t *cenv, const char *prefix)
{
    static const char *cmds[] = {
	"show fish and chips", "show fish and bucket", "show fish and me",
	"show why this works", "show why more or less", "show why more works",
        "show interfaces", "show version", "show arp", NULL
    };
    long delete_before = (long) strlen(prefix);

    for (const char **pc = cmds; *pc != NULL; pc++) {
        if (ic_istarts_with(*pc, prefix)) {
            if (!ic_add_completion_prim(cenv, *pc, NULL, NULL,
                                         delete_before, 0))
                break;
        }
    }
}

/*
 * Key-action test: ctrl-G normally cancels the input outright
 * (clearing it and returning NULL).  Binding it here proves a bound
 * action overrides that built-in handling entirely -- the input
 * survives, and this reports the suppression state instead.
 */
static void
jig_dismiss_action (ic_keyaction_env_t *kenv)
{
    ic_keyaction_dismiss_completions(kenv);
    ic_printf("\n[dismiss: suppressed=%s]\n",
            ic_keyaction_completions_suppressed(kenv) ? "true" : "false");
}

/*
 * Key-action test: report the current suppression state without
 * changing it, so a ".keys" script can observe it being cleared when
 * editing crosses a word boundary (e.g. by typing a space) after a
 * prior ctrl-G.
 */
static void
jig_check_suppressed_action (ic_keyaction_env_t *kenv)
{
    ic_printf("\n[check: suppressed=%s]\n",
            ic_keyaction_completions_suppressed(kenv) ? "true" : "false");
}

/*
 * Key-action test: dump the history through the public accessors
 * ic_history_count()/ic_history_get(), most-recent first.
 */
static void
jig_history_dump_action (ic_keyaction_env_t *kenv)
{
    long count = ic_history_count();

    ic_unused(kenv);
    ic_printf("\n[history: count=%ld]\n", count);
    for (long i = 0; i < count; i += 1)
        ic_printf("[history: %ld: %s]\n", i, ic_history_get(i));
}

/*
 * Key-action test: "?" -- self-insert inside an open quote (so quoted
 * text isn't interrupted), otherwise print the fake completer's
 * matches for the word up to the cursor without inserting anything.
 * Exercises the same generic mechanism sdb's own "?" binding
 * (slaxIoKeyHelp in libslax/slaxio.c) is built on.
 */
static void
jig_help_action (ic_keyaction_env_t *kenv)
{
    static const char *cmds[] = {
        "show interfaces", "show version", "show arp", NULL
    };
    long cursor;
    const char *input = ic_keyaction_input(kenv, &cursor);
    char quote = 0;

    for (long i = 0; i < cursor; i += 1) {
        if (quote != 0) {
            if (input[i] == quote)
                quote = 0;
        } else if (input[i] == '\'' || input[i] == '"') {
            quote = input[i];
        }
    }

    if (quote != 0) {
        ic_keyaction_replace(kenv, "?", 0, 0);
        return;
    }

    ic_printf("\n[help:");
    for (const char **pc = cmds; *pc != NULL; pc += 1) {
        if (strncmp(*pc, input, (size_t) cursor) == 0)
            ic_printf(" %s", *pc);
    }
    ic_printf("]\n");
}

static char *jig_dabbrev_prefix;
static long jig_dabbrev_next;
static long jig_dabbrev_suffixlen;

/*
 * Find the first word in "entry" that is a proper (longer) extension
 * of "prefix".  Returns NULL if there is none.
 */
static const char *
jig_dabbrev_match (const char *entry, const char *prefix, size_t prefixlen,
        size_t *lenp)
{
    const char *cp = entry;

    for (;;) {
        while (*cp != 0 && isspace((unsigned char) *cp))
            cp += 1;

        if (*cp == 0)
            return NULL;

        const char *start = cp;

        while (*cp != 0 && !isspace((unsigned char) *cp))
            cp += 1;

        if ((size_t) (cp - start) > prefixlen
                && strncmp(start, prefix, prefixlen) == 0) {
            *lenp = (size_t) (cp - start);
            return start;
        }
    }
}

/*
 * Key-action test: ESC-/ -- dabbrev-style expansion of the word
 * before the cursor, searching backward through history (skipping
 * history index 0, the in-progress line's own placeholder entry) for
 * a word that properly extends it.  Repeated presses continue the
 * same search, replacing the previous expansion with the next match
 * further back.  Exercises the same generic mechanism sdb's own
 * ESC-/ binding (slaxIoKeyDabbrevExpand) is built on.
 */
static void
jig_dabbrev_action (ic_keyaction_env_t *kenv)
{
    long cursor;
    const char *input = ic_keyaction_input(kenv, &cursor);
    long wordstart = cursor;

    while (wordstart > 0 && !isspace((unsigned char) input[wordstart - 1]))
        wordstart -= 1;

    long wordlen = cursor - wordstart;
    size_t prefixlen = jig_dabbrev_prefix ? strlen(jig_dabbrev_prefix) : 0;
    bool continuing = jig_dabbrev_prefix != NULL
            && (size_t) wordlen == prefixlen + (size_t) jig_dabbrev_suffixlen
            && strncmp(input + wordstart, jig_dabbrev_prefix, prefixlen) == 0;

    if (!continuing) {
        free(jig_dabbrev_prefix);
        jig_dabbrev_prefix = strndup(input + wordstart, (size_t) wordlen);
        jig_dabbrev_next = 1;
        jig_dabbrev_suffixlen = 0;
    }

    prefixlen = strlen(jig_dabbrev_prefix);

    long count = ic_history_count();
    const char *match = NULL;
    size_t matchlen = 0;

    for ( ; jig_dabbrev_next < count; jig_dabbrev_next += 1) {
        const char *entry = ic_history_get(jig_dabbrev_next);

        if (entry == NULL)
            continue;

        match = jig_dabbrev_match(entry, jig_dabbrev_prefix, prefixlen,
                &matchlen);

        if (match != NULL) {
            jig_dabbrev_next += 1;
            break;
        }
    }

    if (match != NULL) {
        size_t suffixlen = matchlen - prefixlen;
        char *suffix = (char *) alloca(suffixlen + 1);

        memcpy(suffix, match + prefixlen, suffixlen);
        suffix[suffixlen] = 0;

        ic_keyaction_replace(kenv, suffix, jig_dabbrev_suffixlen, 0);
        jig_dabbrev_suffixlen = (long) suffixlen;
    }
}

/*
 * Key-action test: ESC-. -- insert the last token of the most recent
 * multi-token history entry, skipping single-token entries and
 * history index 0 (the in-progress line's own placeholder entry).
 * Exercises the same generic mechanism sdb's own ESC-. binding
 * (slaxIoKeyLastArg) is built on.
 */
static void
jig_lastarg_action (ic_keyaction_env_t *kenv)
{
    long count = ic_history_count();

    for (long i = 1; i < count; i += 1) {
        const char *entry = ic_history_get(i);

        if (entry == NULL)
            continue;

        const char *end = entry + strlen(entry);

        while (end > entry && isspace((unsigned char) end[-1]))
            end -= 1;

        const char *start = end;

        while (start > entry && !isspace((unsigned char) start[-1]))
            start -= 1;

        if (start == entry)
            continue;

        size_t len = (size_t) (end - start);
        char *token = (char *) alloca(len + 1);

        memcpy(token, start, len);
        token[len] = 0;

        ic_keyaction_replace(kenv, token, 0, 0);
        return;
    }
}

static void
run_child_readline_loop (const char *path)
{
    ic_set_history(NULL, -1);
    ic_set_default_completer(&jig_completer, NULL);
    ic_enable_transpose_gosling(true);
    ic_bind_key(7, &jig_dismiss_action, NULL);          /* ctrl-G */
    ic_bind_key(22, &jig_check_suppressed_action, NULL); /* ctrl-V */
    ic_bind_key(24, &jig_history_dump_action, NULL);     /* ctrl-X */
    ic_bind_key('?', &jig_help_action, NULL);
    ic_bind_key(IC_KEY_MOD_ALT | '/', &jig_dabbrev_action, NULL);
    ic_bind_key(IC_KEY_MOD_ALT | '.', &jig_lastarg_action, NULL);

    /*
     * The live completion list (step 7) changes what appears on
     * screen for every match, including a single one (no more plain
     * inline hint), so unlike the other features above it is opt-in
     * per test rather than always on -- enabled only for test files
     * whose basename says so, to leave every other ".keys" test's
     * golden output alone.
     */
    const char *base = strrchr(path, '/');

    base = (base == NULL ? path : base + 1);
    if (strncmp(base, "livelist", 8) == 0) {
        ic_enable_completion_live_list(true);
        ic_enable_completion_accept_on_space(true);
    }

    for (;;) {
        char *line = ic_readline("jig> ");

        if (line == NULL)
            break;

        free(line);
    }

    _exit(0);
}

/*
 * No test file given: run ic_readline() directly against the real
 * tty (no pty, no fork, no scripted keys) so a human can drive it by
 * hand -- same completer and live-list/accept-on-space behavior as
 * "livelist.keys", for convenience.
 */
static void
run_interactive (void)
{
    ic_set_history(NULL, -1);
    ic_set_default_completer(&jig_completer, NULL);
    ic_enable_completion_live_list(true);
    ic_enable_completion_accept_on_space(true);

    printf("jig: interactive mode (Ctrl-D to quit)\n");

    for (;;) {
        char *line = ic_readline(">>> ");

        if (line == NULL)
            break;

        printf("%s\n", line);
        free(line);
    }
}

/*
 * Best-effort write to the pty master.  Returns false (and gives up on
 * the rest of the script) if the write fails outright, e.g. because
 * the child has already exited.
 */
static bool
write_all (int fd, const unsigned char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n > 0) {
            off += (size_t) n;
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        return false;
    }

    return true;
}

/*
 * Open a 24x80 pty, fork a child that runs the readline loop against
 * it, then play a ".keys" script against the master side, using the
 * grammar confirmed in the plan:
 *
 *   # comment       ignored
 *   k <keyspec>     send one key (ignored if no keyspec)
 *   + <text>        type text, quotes stripped (ignored if empty)
 *   i <ms>          set persistent inter-key delay (0 clears it)
 *   w <ms>          one-shot wait, does not persist
 *   q               end the script now (EOF does the same)
 *
 * Returns true if the test should be reported as FAILED.  Right now
 * that only means the child under test crashed or exited non-zero on
 * its own, before the script tore the session down -- being killed by
 * us at end-of-script (the normal case: the child is still blocked in
 * ic_readline() waiting for more input) is not a failure.
 */
static bool
run_keys_test (const char *path)
{
    struct winsize ws;

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 24;
    ws.ws_col = 80;

    int master_fd = -1;
    int slave_fd = -1;

    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        perror("jig: openpty");
        return true;
    }

    signal(SIGPIPE, SIG_IGN);

    pid_t pid = fork();

    if (pid < 0) {
        perror("jig: fork");
        return true;
    }

    if (pid == 0) {
        /* child: becomes the session/controlling-tty leader of the slave */
        close(master_fd);

        if (login_tty(slave_fd) < 0) {
            perror("jig: login_tty");
            _exit(127);
        }

        run_child_readline_loop(path);
        _exit(0);
    }

    /* parent */
    close(slave_fd);

    capture_t cap;
    capture_init(&cap);

    drain_arg_t darg;
    darg.da_fd = master_fd;
    darg.da_cap = &cap;

    pthread_t drain_thread;
    pthread_create(&drain_thread, NULL, drain_thread_main, &darg);

    FILE *f = fopen(path, "r");

    if (f == NULL) {
        fprintf(stderr, "jig: cannot open %s: %s\n", path, strerror(errno));
        return true;
    }

    long delay_ms = 0; /* persistent "i" delay */
    char line[1024];
    bool stop = false;

    while (!stop && fgets(line, sizeof(line), f) != NULL) {
        /* strip trailing newline */
        size_t n = strlen(line);

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;

        char *p = line;

        while (*p == ' ' || *p == '\t')
            p += 1;

        if (*p == 0 || *p == '#')
            continue;

        char letter = *p;

        p += 1;
        while (*p == ' ' || *p == '\t')
            p += 1;

        const char *value = p; /* rest of the line; may be empty */

        switch (letter) {
        case 'k':
            if (*value == 0)
                break; /* no value: ignored */

            unsigned char kb[8];
            size_t klen = 0;

            if (encode_key(value, kb, &klen)) {
                if (!write_all(master_fd, kb, klen)) {
                    stop = true;
                    break;
                }
                if (delay_ms > 0)
                    usleep((unsigned int) (delay_ms * 1000));
            } else {
                fprintf(stderr, "jig: %s: unknown key spec: %s\n",
                        path, value);
            }
            break;

        case '+':
            if (strlen(value) == 0)
                break; /* no value: ignored */

            char text[1024];
            size_t tlen = strlen(value);

            if (tlen >= 2 && value[0] == '"' && value[tlen - 1] == '"') {
                tlen -= 2;
                memcpy(text, value + 1, tlen);
            } else {
                memcpy(text, value, tlen);
            }

            for (size_t i = 0; i < tlen; i++) {
                if (!write_all(master_fd, (const unsigned char*) &text[i], 1)) {
                    stop = true;
                    break;
                }
                if (delay_ms > 0)
                    usleep((unsigned int) (delay_ms * 1000));
            }
            break;

        case 'i':
            if (*value == 0)
                break; /* no value: ignored */
            delay_ms = atol(value);
            break;

        case 'w':
            if (*value == 0)
                break; /* no value: ignored */

            long ms = atol(value);

            if (ms > 0)
                usleep((unsigned int) (ms * 1000));
            break;

        case 'q':
            stop = true;
            break;

        default:
            fprintf(stderr, "jig: %s: unknown directive '%c', ignoring line\n",
                    path, letter);
            break;
        }
    }

    fclose(f);

    /*
     * Grace period: let any output still in flight land before we
     * tear the session down.  Reaching EOF on the script acts just
     * like "q".
     */
    usleep(100 * 1000);

    bool child_exited = false;
    int status = 0;

    for (int tries = 0; tries < 50; tries++) {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid) {
            child_exited = true;
            break;
        }
        usleep(20 * 1000);
    }

    if (!child_exited) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    /*
     * Let the drain thread catch any last bytes (e.g. an ASan report
     * written to stderr right before the child's abort()) before we
     * render output.
     */
    usleep(20 * 1000);

    capture_write_escaped(&cap, stdout);

    bool failed = false;

    if (child_exited && WIFSIGNALED(status)) {
        failed = true;
        printf("RESULT: FAIL (child terminated by signal %d: %s)\n",
               WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else if (child_exited && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        failed = true;
        printf("RESULT: FAIL (child exited with status %d)\n",
               WEXITSTATUS(status));
    } else {
        printf("RESULT: PASS\n");
    }

    fflush(stdout);

    return failed;
}

/*
 * Direct-call tests: no pty needed.  These call straight into
 * isocline's private bbcode/attr functions to demonstrate issue #2
 * and issue #3 from build/plans/isocline-issues.rst.
 */

/*
 * Plain-libc alloc_t for the ".direct" tests, so they can build a
 * bbcode_t without going through ic_get_env()/ic_env_create(), which
 * would create a real tty_t/term_t pair and probe the controlling
 * terminal.
 */
static alloc_t direct_test_mem = { malloc, realloc, free };

static bool
run_direct_bug02_attrbuf_delete (void)
{
    /*
     * isocline-issues.rst #2: attrbuf_delete_at() passes an element
     * count where ic_memmove() expects a byte count, so right-aligned
     * width truncation (bbcode_restrict_width()) corrupts the
     * attribute buffer for the surviving characters.  Expect every
     * character of the surviving "defgh" below to be colored green;
     * the bug leaves some of them with stale/garbage attributes
     * instead.
     *
     * Deliberately avoid ic_get_env(): it creates a full tty_t/term_t
     * pair, which puts the real controlling terminal into raw mode
     * and probes it (OSC4 color queries, CPR cursor queries) as a
     * side effect -- visible as garbage on the user's real prompt
     * after this test runs, since only stdout/stderr (not stdin) are
     * redirected to a file here.  bbcode_append() never dereferences
     * bb->term, so a bare alloc_t + bbcode_t (term == NULL) is all it
     * needs.
     */
    bbcode_t *bb = bbcode_new(&direct_test_mem, NULL);
    stringbuf_t *text = sbuf_new(&direct_test_mem);
    attrbuf_t *attrs = attrbuf_new(&direct_test_mem);

    const char *markup =
        "[width=\"5;right\"][color=red]abc[/color]"
        "[color=green]defgh[/color][/width]";

    bbcode_append(bb, markup, text, attrs);

    const uint32_t expect_color =
        attr_from_color(IC_RGB(0x008000)).x.color; /* bbcode "green" */
    bool ok = true;

    printf("input: %s\n", markup);
    printf("text:  %s\n", sbuf_string(text));

    ssize_t n = sbuf_len(text);

    for (ssize_t i = 0; i < n; i++) {
        attr_t a = attrbuf_attr_at(attrs, i);
        bool match = (a.x.color == expect_color);

        if (!match)
            ok = false;

        printf("attr[%zd] char='%c' color=0x%08x "
               "(expected every char green = 0x%08x) %s\n",
               i, sbuf_string(text)[i], a.x.color, expect_color,
               match ? "ok" : "WRONG");
    }

    printf("RESULT: %s\n",
           ok ? "PASS"
              : "FAIL (attribute data corrupted after right-align "
                "truncation)");

    sbuf_free(text);
    attrbuf_free(attrs);
    bbcode_free(bb);

    return !ok;
}

static bool
run_direct_bug03_attr_bool (void)
{
    /*
     * isocline-issues.rst #3: attr_update_bool()'s condition is
     * always true (strcmp() returns 0 on a match, so ORing three
     * such checks together can never be false), so "[bold=off]"
     * always turns bold ON instead of off.  Expect IC_OFF (-1)
     * below; the bug produces IC_ON (1).
     *
     * Deliberately avoid ic_get_env() -- see
     * run_direct_bug02_attrbuf_delete() above for why: it would
     * probe the real controlling terminal.
     */
    bbcode_t *bb = bbcode_new(&direct_test_mem, NULL);
    stringbuf_t *text = sbuf_new(&direct_test_mem);
    attrbuf_t *attrs = attrbuf_new(&direct_test_mem);

    const char *markup = "[bold=off]hello[/bold]";

    bbcode_append(bb, markup, text, attrs);

    printf("input: %s\n", markup);
    printf("text:  %s\n", sbuf_string(text));

    attr_t a = attrbuf_attr_at(attrs, 0);

    printf("attr[0].bold = %d  (IC_OFF=-1 IC_ON=1 IC_NONE=0; "
           "expected IC_OFF)\n", (int) a.x.bold);

    bool ok = (a.x.bold == IC_OFF);

    printf("RESULT: %s\n", ok ? "PASS" : "FAIL (bold is ON instead of OFF)");

    sbuf_free(text);
    attrbuf_free(attrs);
    bbcode_free(bb);

    return !ok;
}

static bool
has_suffix (const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);

    return (ls >= lx && strcmp(s + ls - lx, suffix) == 0);
}

int
main (int argc, char **argv)
{
    int rc;

    if (argc == 1) {
        run_interactive();
        return 0;
    } else if (argc != 2) {
        fprintf(stderr, "usage: %s [test-file]\n", argv[0]);
        rc = 2;
    } else {
        const char *path = argv[1];

        if (has_suffix(path, ".keys")) {
            rc = run_keys_test(path) ? 1 : 0;
        } else if (has_suffix(path, ".direct")) {
            /*
             * Dispatch by basename: these are hardcoded C, the file
             * content itself is just a human-readable marker/comment.
             */
            const char *base = strrchr(path, '/');

            base = (base == NULL ? path : base + 1);

            if (strncmp(base, "bug-02-", 7) == 0) {
                rc = run_direct_bug02_attrbuf_delete() ? 1 : 0;
            } else if (strncmp(base, "bug-03-", 7) == 0) {
                rc = run_direct_bug03_attr_bool() ? 1 : 0;
            } else {
                fprintf(stderr, "jig: no direct test registered for %s\n",
                        path);
                rc = 2;
            }
        } else {
            fprintf(stderr, "jig: %s: unrecognized test file extension\n",
                    path);
            rc = 2;
        }
    }

    /*
     * Guarantee "FAIL" appears in our own (captured) output on any
     * non-zero exit, even for failure modes that never reach one of
     * the "RESULT: FAIL ..." printfs above (bad usage, openpty/fork
     * failure, an unrecognized test file) -- so the check/Makefile's
     * plain diff-against-saved/ output always shows a failure.
     */
    if (rc != 0) {
        printf("FAIL\n");
        fflush(stdout);
    }

    return rc;
}
