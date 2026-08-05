// b2trap_supervisor.c — zero-hot-path-overhead forensics supervisor
// - ptrace parent; target never executes extra instructions in hot path
// - hardware watchpoints (DR0..DR3) optional (-w); pure-dump arm uses none
// - on watchpoint trap: log rip/regs/slot value + classify value neighborhood
// - on fatal signal: bounded dump (target-ish maps + stacks + region meta) or
//   full RW dump for first FULL_DUMP_CAP events (calibration)
#define _GNU_SOURCE
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MAX_WP 4
#define DUMP_MAX_PER_MAP (512UL * 1024 * 1024)
#define FULL_DUMP_CAP_DEFAULT 2
#define BOUNDED_PAD (2UL * 1024 * 1024)
#define MAX_INTEREST 64

static FILE *g_log;
static char g_outdir[512];
static unsigned long g_wp[MAX_WP];
static int g_nwp = 0;
static unsigned long g_hits[MAX_WP];
static unsigned long g_hit_cap = 200000;
static pid_t g_mainpid = -1;
static unsigned long g_exe_lo = 0, g_exe_hi = 0;
static int g_full_dump_cap = FULL_DUMP_CAP_DEFAULT;
static int g_fatal_count = 0;
static int g_force_full = 0; // -D full
static int g_force_bounded = 0; // -D bounded

static void logln(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

static int peek(pid_t pid, unsigned long addr, unsigned long *out) {
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, 0);
    if (errno != 0)
        return -1;
    *out = (unsigned long)v;
    return 0;
}

static void read_maps(pid_t pid, const char *tag) {
    char p[64], q[600];
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    snprintf(q, sizeof q, "%s/maps_%s_%d.txt", g_outdir, tag, pid);
    int in = open(p, O_RDONLY);
    int out = open(q, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) {
        logln("READ_MAPS_FAIL pid=%d in=%d out=%d errno=%d path=%s", pid, in, out, errno, q);
        if (in >= 0)
            close(in);
        if (out >= 0)
            close(out);
        return;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t w_ = write(out, buf, n);
        (void)w_;
    }
    close(in);
    close(out);
}

static void learn_exe_range(pid_t pid, const char *exe_base) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[512];
    size_t elen = strlen(exe_base);
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
            continue;
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == ' '))
            line[--l] = 0;
        if (l >= elen && strcmp(line + l - elen, exe_base) == 0) {
            if (!g_exe_lo || lo < g_exe_lo)
                g_exe_lo = lo;
            if (hi > g_exe_hi)
                g_exe_hi = hi;
        }
    }
    fclose(f);
    logln("EXE_RANGE %lx-%lx", g_exe_lo, g_exe_hi);
}

static int is_exe_ptr(unsigned long v) { return g_exe_lo && v >= g_exe_lo && v < g_exe_hi; }

static const char *classify_value(pid_t pid, unsigned long v, unsigned long *tip_at, unsigned long *tip_before) {
    *tip_at = 0;
    *tip_before = 0;
    if (v < 0x10000)
        return "null_or_low";
    if (is_exe_ptr(v))
        return "code_or_static";
    if ((v >> 40) != 0x7f)
        return "other";
    if (peek(pid, v, tip_at) == 0 && is_exe_ptr(*tip_at))
        return "base_obj_at_v";
    if (peek(pid, v - 16, tip_before) == 0 && is_exe_ptr(*tip_before))
        return "interior_plus16";
    if (peek(pid, v - 8, tip_before) == 0 && is_exe_ptr(*tip_before))
        return "interior_plus8";
    return "unclassified_heap";
}

static void set_watchpoints(pid_t pid) {
    for (int i = 0; i < g_nwp; i++) {
        if (ptrace(PTRACE_POKEUSER, pid, (void *)(offsetof(struct user, u_debugreg[0]) + i * sizeof(unsigned long)),
                   (void *)g_wp[i]) != 0)
            logln("WP_SET_FAIL pid=%d i=%d errno=%d", pid, i, errno);
    }
    unsigned long dr7 = 0;
    for (int i = 0; i < g_nwp; i++)
        dr7 |= (1UL << (2 * i)) | (0x1UL << (16 + 4 * i)) | (0x2UL << (18 + 4 * i)); // write, 8 bytes
    if (ptrace(PTRACE_POKEUSER, pid, (void *)offsetof(struct user, u_debugreg[7]), (void *)dr7) != 0)
        logln("DR7_SET_FAIL pid=%d errno=%d", pid, errno);
    else
        logln("WP_ARMED pid=%d n=%d dr7=%#lx", pid, g_nwp, dr7);
}

// DRs are NOT inherited across clone on Linux (verified empirically 2026-08-05):
// every newly attached thread must be armed at its initial stop before it runs user code.
#define MAX_ARMED 512
static pid_t g_armed[MAX_ARMED];
static int g_narmed_t = 0;
static void maybe_arm(pid_t tid) {
    if (!g_nwp)
        return;
    for (int i = 0; i < g_narmed_t; i++)
        if (g_armed[i] == tid)
            return;
    set_watchpoints(tid);
    if (g_narmed_t < MAX_ARMED)
        g_armed[g_narmed_t++] = tid;
}

static void clear_dr(pid_t pid) {
    ptrace(PTRACE_POKEUSER, pid, (void *)offsetof(struct user, u_debugreg[7]), (void *)0);
}

static int dump_range(pid_t pid, unsigned long lo, unsigned long hi, const char *tag) {
    if (hi <= lo)
        return 0;
    unsigned long sz = hi - lo;
    if (sz > DUMP_MAX_PER_MAP)
        sz = DUMP_MAX_PER_MAP;
    char q[600];
    snprintf(q, sizeof q, "%s/mem_%s_%lx-%lx.bin", g_outdir, tag, lo, hi);
    int out = open(q, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        logln("DUMP_OPEN_FAIL %s errno=%d", q, errno);
        return 0;
    }
    unsigned long done = 0;
    static char buf[1 << 20];
    while (done < sz) {
        struct iovec li = {buf, 0}, ri = {(void *)(lo + done), 0};
        size_t want = sz - done;
        if (want > sizeof buf)
            want = sizeof buf;
        li.iov_len = want;
        ri.iov_len = want;
        ssize_t r = process_vm_readv(pid, &li, 1, &ri, 1, 0);
        if (r <= 0)
            break;
        ssize_t w_ = write(out, buf, r);
        (void)w_;
        done += (unsigned long)r;
    }
    close(out);
    logln("DUMP_RANGE tag=%s lo=%#lx hi=%#lx got=%lu", tag, lo, lo + sz, done);
    return 1;
}

static int map_should_skip_path(const char *path) {
    if (!path || !path[0])
        return 0;
    if (strstr(path, ".so") || strstr(path, "llvm") || strstr(path, "/usr/") || strstr(path, "/lib"))
        return 1;
    return 0;
}

static void dump_all_memory(pid_t pid, const char *tag) {
    read_maps(pid, tag);
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) {
        logln("DUMP_MAPS_OPEN_FAIL pid=%d errno=%d", pid, errno);
        return;
    }
    int ndump = 0;
    unsigned long total = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8] = {0};
        unsigned long long off;
        char path[256] = {0};
        int n = sscanf(line, "%lx-%lx %4s %llx %*s %*s %255[^\n]", &lo, &hi, perm, &off, path);
        if (n < 4)
            continue;
        if (perm[0] != 'r' || perm[1] != 'w')
            continue;
        if (map_should_skip_path(path))
            continue;
        if (ndump == 0)
            logln("DUMP_FIRST_FILE full mode");
        if (dump_range(pid, lo, hi, tag)) {
            ndump++;
            total += (hi - lo > DUMP_MAX_PER_MAP) ? DUMP_MAX_PER_MAP : (hi - lo);
        }
    }
    fclose(f);
    logln("DUMP_MAPS_DONE mode=full pid=%d ndump=%d bytes≈%lu", pid, ndump, total);
}

// Bounded dump: stacks + maps covering interest addresses (regs / B2GEO) + neighbor pads.
// Also keeps large anonymous RW maps that look like region-metadata arenas (small pages
// with many RegionInfo) by dumping maps that contain (interest - BOUNDED_PAD).
static void dump_bounded_memory(pid_t pid, const char *tag, unsigned long *interest, int ninterest) {
    read_maps(pid, tag);
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) {
        logln("DUMP_MAPS_OPEN_FAIL pid=%d errno=%d", pid, errno);
        return;
    }
    int ndump = 0;
    unsigned long total = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8] = {0};
        unsigned long long off;
        char path[256] = {0};
        int n = sscanf(line, "%lx-%lx %4s %llx %*s %*s %255[^\n]", &lo, &hi, perm, &off, path);
        if (n < 4)
            continue;
        if (perm[0] != 'r')
            continue;
        int is_stack = (path[0] && (strstr(path, "[stack") || strstr(path, "stack")));
        int is_rw = (perm[1] == 'w');
        if (!is_stack && !is_rw)
            continue;
        if (!is_stack && map_should_skip_path(path))
            continue;

        int take = is_stack;
        unsigned long dlo = lo, dhi = hi;
        if (!take) {
            for (int i = 0; i < ninterest; i++) {
                unsigned long a = interest[i];
                if (!a)
                    continue;
                // map contains address, or is within pad of address (neighbor region)
                if ((a >= lo && a < hi) || (a + BOUNDED_PAD >= lo && a < hi + BOUNDED_PAD)) {
                    take = 1;
                    // clip to pad around interest if map is huge
                    if (hi - lo > 8UL * 1024 * 1024) {
                        unsigned long clo = (a > BOUNDED_PAD) ? (a - BOUNDED_PAD) : lo;
                        unsigned long chi = a + BOUNDED_PAD;
                        if (clo < lo)
                            clo = lo;
                        if (chi > hi)
                            chi = hi;
                        // expand to cover whole map if map is a single 64K region page band
                        if (hi - lo <= 256UL * 1024) {
                            clo = lo;
                            chi = hi;
                        }
                        dlo = clo;
                        dhi = chi;
                    }
                    break;
                }
            }
        }
        // Always take anonymous RW maps in the typical cj heap/meta band that are
        // modest sized (region tables / small heaps) — catches region metadata.
        if (!take && is_rw && !path[0] && (lo >> 40) == 0x7f && (hi - lo) <= 16UL * 1024 * 1024) {
            // only if any interest is in same 1GB superpage neighborhood
            for (int i = 0; i < ninterest; i++) {
                unsigned long a = interest[i];
                if (!a)
                    continue;
                if ((a ^ lo) < (1UL << 30)) {
                    take = 1;
                    dlo = lo;
                    dhi = hi;
                    break;
                }
            }
        }
        if (!take)
            continue;
        if (dump_range(pid, dlo, dhi, tag)) {
            ndump++;
            total += dhi - dlo;
        }
    }
    fclose(f);
    logln("DUMP_MAPS_DONE mode=bounded pid=%d ndump=%d bytes≈%lu ninterest=%d", pid, ndump, total, ninterest);
}

static void collect_interest_from_regs(pid_t tid, unsigned long *interest, int *ninterest) {
    struct user_regs_struct r;
    if (ptrace(PTRACE_GETREGS, tid, 0, &r) != 0)
        return;
    unsigned long regs[16] = {r.rax, r.rbx, r.rcx, r.rdx, r.rsi, r.rdi, r.rbp, r.rsp,
                              r.r8,  r.r9,  r.r10, r.r11, r.r12, r.r13, r.r14, r.r15};
    for (int i = 0; i < 16 && *ninterest < MAX_INTEREST; i++) {
        unsigned long v = regs[i];
        if ((v >> 40) == 0x7f || (v >> 40) == 0x55) {
            interest[(*ninterest)++] = v;
            // also track V-16 / V+16 for interior geometry
            if (*ninterest < MAX_INTEREST && v > 16)
                interest[(*ninterest)++] = v - 16;
            if (*ninterest < MAX_INTEREST)
                interest[(*ninterest)++] = v + 16;
        }
    }
}

static void dump_thread_regs(pid_t tid, const char *tag) {
    struct user_regs_struct r;
    if (ptrace(PTRACE_GETREGS, tid, 0, &r) != 0)
        return;
    logln("REGS %s tid=%d rip=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx "
          "r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx",
          tag, tid, r.rip, r.rax, r.rbx, r.rcx, r.rdx, r.rsi, r.rdi, r.rbp, r.rsp, r.r8, r.r9, r.r10, r.r11, r.r12,
          r.r13, r.r14, r.r15);
    // T-A: scan GP regs for the B2 geometry: V where *V points into exe TEXT (code as "TypeInfo")
    // while *(V-16) points into exe range (legit TypeInfo one slot before) = (obj+0x10)-as-base shape.
    unsigned long regs[16] = {r.rax, r.rbx, r.rcx, r.rdx, r.rsi, r.rdi, r.rbp, r.rsp,
                              r.r8,  r.r9,  r.r10, r.r11, r.r12, r.r13, r.r14, r.r15};
    const char *names[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                             "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; i++) {
        unsigned long v = regs[i];
        if ((v >> 40) != 0x7f)
            continue;
        unsigned long at = 0, before = 0;
        if (peek(tid, v, &at) != 0)
            continue;
        int at_exe = is_exe_ptr(at);
        if (peek(tid, v - 16, &before) == 0 && is_exe_ptr(before) && at_exe) {
            logln("B2GEO_CANDIDATE reg=%s V=%#lx *V=%#lx *(V-16)=%#lx", names[i], v, at, before);
        } else if (at_exe) {
            logln("GEO_PTR reg=%s V=%#lx *V=%#lx(exe) *(V-16)=%#lx", names[i], v, at, before);
        } else if (peek(tid, v - 16, &before) == 0 && is_exe_ptr(before)) {
            logln("INTERIOR16_REG reg=%s V=%#lx *V=%#lx *(V-16)=%#lx(exe)", names[i], v, at, before);
        }
    }
}

static void on_fatal(pid_t tid, int sig) {
    const char *stag = sig == SIGABRT ? "abrt" : sig == SIGSEGV ? "segv" : "fatal";
    logln("FATAL_SIGNAL tid=%d sig=%d -> dumping", tid, sig);
    dump_thread_regs(tid, "fatal");

    unsigned long interest[MAX_INTEREST];
    int ninterest = 0;
    collect_interest_from_regs(tid, interest, &ninterest);

    g_fatal_count++;
    int do_full = g_force_full || (!g_force_bounded && g_fatal_count <= g_full_dump_cap);
    logln("DUMP_POLICY fatal_n=%d full_cap=%d mode=%s ninterest=%d", g_fatal_count, g_full_dump_cap,
          do_full ? "full" : "bounded", ninterest);
    if (do_full)
        dump_all_memory(tid, stag);
    else
        dump_bounded_memory(tid, stag, interest, ninterest);
    logln("DUMP_DONE tid=%d sig=%d", tid, sig);
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    const char *outdir = ".";
    int timeout_s = 120;
    static char exe_path[512];
    exe_path[0] = 0;
    int a = 1;
    while (a < argc && argv[a][0] == '-') {
        if (!strcmp(argv[a], "-o")) {
            outdir = argv[a + 1];
            a += 2;
        } else if (!strcmp(argv[a], "-t")) {
            timeout_s = atoi(argv[a + 1]);
            a += 2;
        } else if (!strcmp(argv[a], "-c")) {
            g_hit_cap = strtoul(argv[a + 1], 0, 0);
            a += 2;
        } else if (!strcmp(argv[a], "-F")) {
            g_full_dump_cap = atoi(argv[a + 1]);
            a += 2;
        } else if (!strcmp(argv[a], "-D")) {
            if (!strcmp(argv[a + 1], "full"))
                g_force_full = 1;
            else if (!strcmp(argv[a + 1], "bounded"))
                g_force_bounded = 1;
            else {
                fprintf(stderr, "bad -D %s (full|bounded)\n", argv[a + 1]);
                return 2;
            }
            a += 2;
        } else if (!strcmp(argv[a], "-w")) {
            if (g_nwp >= MAX_WP) {
                fprintf(stderr, "too many -w\n");
                return 2;
            }
            g_wp[g_nwp] = strtoul(argv[a + 1], 0, 0);
            if (g_wp[g_nwp] % 8) {
                fprintf(stderr, "watchpoint addr must be 8-aligned\n");
                return 2;
            }
            g_nwp++;
            a += 2;
        } else if (!strcmp(argv[a], "--")) {
            a++;
            break;
        } else {
            fprintf(stderr, "unknown opt %s\n", argv[a]);
            return 2;
        }
    }
    if (a >= argc) {
        fprintf(stderr, "usage: supervisor [-o dir] [-t s] [-c cap] [-F full_cap] [-D full|bounded] [-w addr].. -- "
                        "target [args]\n");
        return 2;
    }
    snprintf(exe_path, sizeof exe_path, "%s", argv[a]);
    const char *base = strrchr(exe_path, '/');
    base = base ? base + 1 : exe_path;
    snprintf(g_outdir, sizeof g_outdir, "%s", outdir);

    char lp[600];
    snprintf(lp, sizeof lp, "%s/supervisor.log", outdir);
    g_log = fopen(lp, "w");
    if (!g_log) {
        perror("log");
        return 2;
    }
    char cwdbuf[512];
    if (getcwd(cwdbuf, sizeof cwdbuf))
        logln("SUPERVISOR_CWD=%s", cwdbuf);

    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        personality(ADDR_NO_RANDOMIZE | personality(0xffffffffUL));
        execv(argv[a], &argv[a]);
        _exit(127);
    }
    g_mainpid = pid;
    logln("SUPERVISOR_START mainpid=%d nwp=%d target=%s full_cap=%d", pid, g_nwp, exe_path, g_full_dump_cap);
    for (int i = 0; i < g_nwp; i++)
        logln("WP_DEF %d addr=%#lx", i, g_wp[i]);

    long opts = PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT;
    int status;
    time_t t0 = time(0);
    int live = 1, exit_code = -1, termsig = 0, fatal_seen = 0, exec_armed = 0;
    // first stop after TRACEME is the post-exec SIGTRAP: maps are already the target's.
    waitpid(pid, &status, 0);
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)opts);
    learn_exe_range(pid, base);
    read_maps(pid, "exec");
    if (g_nwp)
        set_watchpoints(pid);
    exec_armed = 1;
    logln("EXEC_ARMED tid=%d", pid);
    ptrace(PTRACE_CONT, pid, 0, 0);

    while (live > 0) {
        if (time(0) - t0 > timeout_s) {
            logln("TIMEOUT killing");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        pid_t w = waitpid(-1, &status, __WALL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(status)) {
            logln("EXIT tid=%d code=%d", w, WEXITSTATUS(status));
            if (w == g_mainpid)
                exit_code = WEXITSTATUS(status);
            live--;
            continue;
        }
        if (WIFSIGNALED(status)) {
            logln("KILLED tid=%d sig=%d", w, WTERMSIG(status));
            if (w == g_mainpid)
                termsig = WTERMSIG(status);
            live--;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;
        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xffff;
        if (exec_armed && !event)
            maybe_arm(w);

        if (event == PTRACE_EVENT_EXEC) {
            // setarch / wrappers re-exec the real target — always re-learn exe range
            char newbase[256] = {0};
            char linkp[64], realp[512];
            snprintf(linkp, sizeof linkp, "/proc/%d/exe", w);
            ssize_t rl = readlink(linkp, realp, sizeof realp - 1);
            if (rl > 0) {
                realp[rl] = 0;
                const char *nb = strrchr(realp, '/');
                nb = nb ? nb + 1 : realp;
                snprintf(newbase, sizeof newbase, "%s", nb);
            } else {
                snprintf(newbase, sizeof newbase, "%s", base);
            }
            g_exe_lo = g_exe_hi = 0;
            learn_exe_range(w, newbase);
            read_maps(w, "exec");
            if (g_nwp)
                set_watchpoints(w);
            exec_armed = 1;
            logln("EXEC_ARMED tid=%d exe=%s", w, newbase);
            ptrace(PTRACE_CONT, w, 0, 0);
            continue;
        }
        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK) {
            unsigned long newtid = 0;
            ptrace(PTRACE_GETEVENTMSG, w, 0, &newtid);
            logln("CLONE tid=%d new=%lu", w, newtid);
            live++;
            ptrace(PTRACE_CONT, w, 0, 0);
            continue;
        }
        if (event == PTRACE_EVENT_EXIT) {
            ptrace(PTRACE_CONT, w, 0, 0);
            continue;
        }

        if (sig == SIGTRAP) {
            siginfo_t si;
            si.si_code = 0;
            ptrace(PTRACE_GETSIGINFO, w, 0, &si);
            if (si.si_code == TRAP_HWBKPT) {
                errno = 0;
                long d6 = ptrace(PTRACE_PEEKUSER, w, (void *)offsetof(struct user, u_debugreg[6]), 0);
                unsigned long dr6 = errno ? 0 : (unsigned long)d6;
                struct user_regs_struct r;
                ptrace(PTRACE_GETREGS, w, 0, &r);
                for (int i = 0; i < g_nwp; i++) {
                    if (!(dr6 & (1UL << i)))
                        continue;
                    g_hits[i]++;
                    unsigned long slotval = 0;
                    int ok = (peek(w, g_wp[i], &slotval) == 0);
                    if (g_hits[i] <= 100000) {
                        unsigned long tip_at = 0, tip_before = 0;
                        const char *cls = ok ? classify_value(w, slotval, &tip_at, &tip_before) : "peek_fail";
                        logln("WP_HIT i=%d tid=%d rip=%llx slot=%#lx val=%#lx cls=%s tip_at=%#lx tip_before=%#lx hit=%lu",
                              i, w, r.rip, g_wp[i], slotval, cls, tip_at, tip_before, g_hits[i]);
                    }
                    if (g_hits[i] == g_hit_cap) {
                        logln("WP_CAP_REACHED i=%d slot=%#lx hits=%lu -> clearing DRs in this thread", i, g_wp[i],
                              g_hits[i]);
                        clear_dr(w);
                    }
                }
                ptrace(PTRACE_POKEUSER, w, (void *)offsetof(struct user, u_debugreg[6]), (void *)0);
                ptrace(PTRACE_CONT, w, 0, 0);
                continue;
            }
            ptrace(PTRACE_CONT, w, 0, (void *)(long)sig);
            continue;
        }

        if (sig == SIGABRT || sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGBUS) {
            on_fatal(w, sig);
            fatal_seen = 1;
            ptrace(PTRACE_CONT, w, 0, (void *)(long)sig);
            continue;
        }

        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
            ptrace(PTRACE_CONT, w, 0, 0);
            continue;
        }

        ptrace(PTRACE_CONT, w, 0, (void *)(long)sig);
    }
    logln("SUPERVISOR_END exit=%d termsig=%d fatal_seen=%d fatal_count=%d", exit_code, termsig, fatal_seen,
          g_fatal_count);
    fclose(g_log);
    // surface target fate to parent classifier (128+sig convention)
    if (termsig > 0)
        return 128 + termsig;
    if (exit_code >= 0)
        return exit_code;
    return fatal_seen ? 134 : 1;
}
