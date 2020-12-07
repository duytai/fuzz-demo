#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <libgen.h>

#include "config.h"
#include "debug.h"
#include "alloc-inl.h"
#include "types.h"

typedef struct {
  u8* fname;
  u32 len;
} queue_entry;

typedef struct {
  queue_entry entries[10000000];
  u32 size;
} queue;

enum {
  /* 00 */ FAULT_NONE,
  /* 01 */ FAULT_TMOUT,
  /* 02 */ FAULT_CRASH,
  /* 03 */ FAULT_ERROR,
  /* 04 */ FAULT_NOINST,
  /* 05 */ FAULT_NOBITS
};

u8 *in_dir,
   *map_file,
   *report_file,
   *trace_dir,
   *branch_dir,
   *trace_bits,
   *out_file,
   *target_path,
   kill_signal,
   child_timed_out,
   virgin_crash[MAP_SIZE],
   virgin_bits[MAP_SIZE];
u32 exec_tmout = EXEC_TIMEOUT;
s32 shm_id,
    out_fd,
    child_pid,
    forksrv_pid,
    dev_null_fd,
    dev_urandom_fd,
    fsrv_ctl_fd,
    fsrv_st_fd;
u64 mem_limit = MEM_LIMIT;
queue qu;

const u8 count_class_lookup8[256] = {
  [0]           = 0,
  [1]           = 1,
  [2]           = 2,
  [3]           = 4,
  [4 ... 7]     = 8,
  [8 ... 15]    = 16,
  [16 ... 31]   = 32,
  [32 ... 127]  = 64,
  [128 ... 255] = 128
};
u16 count_class_lookup16[65536];

void init_count_class16(void) {
  u32 b1, b2;
  for (b1 = 0; b1 < 256; b1++)
    for (b2 = 0; b2 < 256; b2++)
      count_class_lookup16[(b1 << 8) + b2] =
        (count_class_lookup8[b1] << 8) |
        count_class_lookup8[b2];
}

void detect_file_args(char** argv) {
  u32 i = 0;
  u8* cwd = getcwd(NULL, 0);

  if (!cwd) PFATAL("getcwd() failed");
  while (argv[i]) {
    u8* aa_loc = strstr(argv[i], "@@");
    if (aa_loc) {
      u8 *aa_subst, *n_arg;
      /* If we don't have a file name chosen yet, use a safe default. */
      if (!out_file) out_file = "/tmp/.cur_input";
      /* Be sure that we're always using fully-qualified paths. */
      if (out_file[0] == '/') aa_subst = out_file;
      else aa_subst = alloc_printf("%s/%s", cwd, out_file);
      /* Construct a replacement argv value. */
      *aa_loc = 0;
      n_arg = alloc_printf("%s%s%s", argv[i], aa_subst, aa_loc + 2);
      argv[i] = n_arg;
      *aa_loc = '@';
      if (out_file[0] != '/') ck_free(aa_subst);
    }
    i++;
  }
  free(cwd); /* not tracked */
}

void remove_shm(void) {
  shmctl(shm_id, IPC_RMID, NULL);
}

void setup_shm(void) {
  u8* shm_str;
  shm_id = shmget(IPC_PRIVATE, MAP_SIZE * 9, IPC_CREAT | IPC_EXCL | 0600);
  if (shm_id < 0) PFATAL("shmget() failed");
  atexit(remove_shm);
  shm_str = alloc_printf("%d", shm_id);
  setenv(SHM_ENV_VAR, shm_str, 1);
  ck_free(shm_str);
  trace_bits = shmat(shm_id, NULL, 0);
  if (trace_bits == (void *)-1) PFATAL("shmat() failed");
  memset(virgin_bits, 255, MAP_SIZE);
  memset(virgin_crash, 255, MAP_SIZE);
}

void setup_fds(void) {
  dev_null_fd = open("/dev/null", O_RDWR);
  if (dev_null_fd < 0) PFATAL("Unable to open /dev/null");

  dev_urandom_fd = open("/dev/urandom", O_RDONLY);
  if (dev_urandom_fd < 0) PFATAL("Unable to open /dev/urandom");

  if (trace_dir) mkdir(trace_dir, 0700);
  if (branch_dir) mkdir(branch_dir, 0700);
}

void read_args(int argc, char** argv) {
  s32 opt;
  while ((opt = getopt(argc,argv,"+i:t:b:m:r:")) > 0) {
    switch (opt) {
      case 'r':
        report_file = optarg;
        break;
      case 'm':
        map_file = optarg;
        break;
      case 'i':
        in_dir = optarg;
        break;
      case 'b':
        branch_dir = optarg;
        break;
      case 't':
        trace_dir = optarg;
        break;
      default:
        SAYF("%s -i <in_dir> <app>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
  }

  if (optind == argc || !in_dir) {
    SAYF("%s -i <in_dir> -o <out_dur> <app>\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  detect_file_args(argv + optind + 1);
  target_path = ck_strdup(argv[optind]);;
}

void handle_stop_sig(int sig) {
  if (child_pid > 0) kill(child_pid, SIGKILL);
  if (forksrv_pid > 0) kill(forksrv_pid, SIGKILL);
}

void handle_timeout(int sig) {
  if (child_pid > 0) {
    child_timed_out = 1;
    kill(child_pid, SIGKILL);
  } else if (child_pid == -1 && forksrv_pid > 0) {
    child_timed_out = 1;
    kill(forksrv_pid, SIGKILL);
  }
}

void setup_signal_handlers(void) {
  struct sigaction sa;
  sa.sa_handler   = NULL;
  sa.sa_flags     = SA_RESTART;
  sa.sa_sigaction = NULL;
  sigemptyset(&sa.sa_mask);
  /* Various ways of saying "stop". */
  sa.sa_handler = handle_stop_sig;
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  /* Exec timeout notifications. */
  sa.sa_handler = handle_timeout;
  sigaction(SIGALRM, &sa, NULL);
  /* Things we don't care about. */
  sa.sa_handler = SIG_IGN;
  sigaction(SIGTSTP, &sa, NULL);
  sigaction(SIGPIPE, &sa, NULL);
}

void init_forkserver(char** argv) {

  static struct itimerval it;
  int st_pipe[2], ctl_pipe[2];
  int status;
  s32 rlen;

  ACTF("Spinning up the fork server...");
  if (pipe(st_pipe) || pipe(ctl_pipe)) PFATAL("pipe() failed");
  forksrv_pid = fork();
  if (forksrv_pid < 0) PFATAL("fork() failed");
  if (!forksrv_pid) {
    struct rlimit r;
    if (!getrlimit(RLIMIT_NOFILE, &r) && r.rlim_cur < FORKSRV_FD + 2) {
      r.rlim_cur = FORKSRV_FD + 2;
      setrlimit(RLIMIT_NOFILE, &r); /* Ignore errors */
    }
    if (mem_limit) {
      r.rlim_max = r.rlim_cur = ((rlim_t)mem_limit) << 20;
      setrlimit(RLIMIT_AS, &r); /* Ignore errors */
    }
    r.rlim_max = r.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &r); /* Ignore errors */
    setsid();

    dup2(dev_null_fd, 1);
    dup2(dev_null_fd, 2);

    if (out_file) {
      dup2(dev_null_fd, 0);
    } else {
      dup2(out_fd, 0);
      close(out_fd);
    }

    if (dup2(ctl_pipe[0], FORKSRV_FD) < 0) PFATAL("dup2() failed");
    if (dup2(st_pipe[1], FORKSRV_FD + 1) < 0) PFATAL("dup2() failed");

    close(ctl_pipe[0]);
    close(ctl_pipe[1]);
    close(st_pipe[0]);
    close(st_pipe[1]);

    close(dev_null_fd);
    close(dev_urandom_fd);

    if (!getenv("LD_BIND_LAZY")) setenv("LD_BIND_NOW", "1", 0);
    setenv("ASAN_OPTIONS", "abort_on_error=1:"
        "detect_leaks=0:"
        "symbolize=0:"
        "allocator_may_return_null=1", 0);
    setenv("MSAN_OPTIONS", "exit_code=" STRINGIFY(MSAN_ERROR) ":"
        "symbolize=0:"
        "abort_on_error=1:"
        "allocator_may_return_null=1:"
        "msan_track_origins=0", 0);
    execv(target_path, argv);
    *(u32*)trace_bits = EXEC_FAIL_SIG;
    exit(0);
  }

  close(ctl_pipe[0]);
  close(st_pipe[1]);

  fsrv_ctl_fd = ctl_pipe[1];
  fsrv_st_fd  = st_pipe[0];

  it.it_value.tv_sec = ((exec_tmout * FORK_WAIT_MULT) / 1000);
  it.it_value.tv_usec = ((exec_tmout * FORK_WAIT_MULT) % 1000) * 1000;
  setitimer(ITIMER_REAL, &it, NULL);

  rlen = read(fsrv_st_fd, &status, 4);

  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 0;
  setitimer(ITIMER_REAL, &it, NULL);

  if (rlen == 4) {
    OKF("All right - fork server is up.");
    return;
  }
  FATAL("Fork server handshake failed");
}

void classify_counts(u64* mem) {
  u32 i = MAP_SIZE >> 3;
  while (i--) {
    /* Optimize for sparse bitmaps. */
    if (unlikely(*mem)) {
      u16* mem16 = (u16*)mem;
      mem16[0] = count_class_lookup16[mem16[0]];
      mem16[1] = count_class_lookup16[mem16[1]];
      mem16[2] = count_class_lookup16[mem16[2]];
      mem16[3] = count_class_lookup16[mem16[3]];
    }
    mem++;
  }
}

u8 run_target(char** argv, u32 timeout) {

  static struct itimerval it;
  static u32 prev_timed_out = 0;
  static u64 exec_ms = 0;

  int status = 0;
  u32 tb4;

  child_timed_out = 0;
  memset(trace_bits, 0, MAP_SIZE * 9);
  MEM_BARRIER();

  s32 res;

  if ((res = write(fsrv_ctl_fd, &prev_timed_out, 4)) != 4) {
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");
  }
  if ((res = read(fsrv_st_fd, &child_pid, 4)) != 4) {
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");
  }
  if (child_pid <= 0) FATAL("Fork server is misbehaving (OOM?)");
  /* Configure timeout, as requested by user, then wait for child to terminate. */
  it.it_value.tv_sec = (timeout / 1000);
  it.it_value.tv_usec = (timeout % 1000) * 1000;
  setitimer(ITIMER_REAL, &it, NULL);
  /* The SIGALRM handler simply kills the child_pid and sets child_timed_out. */
  if ((res = read(fsrv_st_fd, &status, 4)) != 4) {
    RPFATAL(res, "Unable to communicate with fork server (OOM?)");
  }
  if (!WIFSTOPPED(status)) child_pid = 0;

  getitimer(ITIMER_REAL, &it);
  exec_ms = (u64) timeout - (it.it_value.tv_sec * 1000 +
      it.it_value.tv_usec / 1000);

  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 0;

  setitimer(ITIMER_REAL, &it, NULL);
  MEM_BARRIER();
  tb4 = *(u32*)trace_bits;
  classify_counts((u64*)trace_bits);
  prev_timed_out = child_timed_out;
  /* Report outcome to caller. */
  if (WIFSIGNALED(status)) {
    kill_signal = WTERMSIG(status);
    if (child_timed_out && kill_signal == SIGKILL) return FAULT_TMOUT;
    return FAULT_CRASH;
  }
  /* It makes sense to account for the slowest units only if the testcase was run
     under the user defined timeout. */
  return FAULT_NONE;
}

u8 has_new_bits(u8* virgin_map) {
  u64* current = (u64*)trace_bits;
  u64* virgin = (u64*)virgin_map;

  u32 i = (MAP_SIZE >> 3);
  u8 ret = 0;
  while (i--) {
    if (unlikely(*current) && unlikely(*current & *virgin)) {
      if (likely(ret < 2)) {
        u8* cur = (u8*)current;
        u8* vir = (u8*)virgin;
        /* Looks like we have not found any new bytes yet; see if any non-zero
           bytes in current[] are pristine in virgin[]. */
        if ((cur[0] && vir[0] == 0xff) || (cur[1] && vir[1] == 0xff) ||
            (cur[2] && vir[2] == 0xff) || (cur[3] && vir[3] == 0xff) ||
            (cur[4] && vir[4] == 0xff) || (cur[5] && vir[5] == 0xff) ||
            (cur[6] && vir[6] == 0xff) || (cur[7] && vir[7] == 0xff)) ret = 2;
        else ret = 1;
      }
      *virgin &= ~*current;
    }
    current++;
    virgin++;
  }
  return ret;
}

void setup_stdio_file(void) {
  u8* fn = "/tmp/.cur_input";
  unlink(fn); /* Ignore errors */
  out_fd = open(fn, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (out_fd < 0) PFATAL("Unable to create '%s'", fn);
}

void write_to_testcase(void* mem, u32 len) {
  s32 fd = out_fd;
  if (out_file) {
    unlink(out_file); /* Ignore errors. */
    fd = open(out_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) PFATAL("Unable to create '%s'", out_file);
  } else lseek(fd, 0, SEEK_SET);
  ck_write(fd, mem, len, out_file);
  if (!out_file) {
    if (ftruncate(fd, len)) PFATAL("ftruncate() failed");
    lseek(fd, 0, SEEK_SET);
  } else close(fd);
}

void read_testcases(void) {
  struct dirent **nl;
  s32 nl_cnt;
  u32 i;

  /* Auto-detect non-in-place resumption attempts. */
  ACTF("Scanning '%s'...", in_dir);
  nl_cnt = scandir(in_dir, &nl, NULL, alphasort);
  for (i = 0; i < nl_cnt; i++) {
    struct stat st;
    u8* fn = alloc_printf("%s/%s", in_dir, nl[i]->d_name);
    free(nl[i]); /* not tracked */
    if (lstat(fn, &st) || access(fn, R_OK))
      PFATAL("Unable to access '%s'", fn);
    /* This also takes care of . and .. */
    if (!S_ISREG(st.st_mode) || !st.st_size || strstr(fn, "/README.txt")) {
      ck_free(fn);
      continue;
    }
    if (st.st_size > MAX_FILE) FATAL("Test case '%s' is too big", fn);
    queue_entry entry;
    entry.fname = strdup(fn);
    entry.len = st.st_size;
    qu.entries[qu.size] = entry;
    qu.size ++;
    ck_free(fn);
  }
  free(nl); /* not tracked */
}

int main(int argc, char** argv) {
  char ** use_argv;
  s32 fd, report_fd;

  read_args(argc, argv);
  if (!out_file) setup_stdio_file();
  setup_signal_handlers();
  setup_shm();
  init_count_class16();
  use_argv = argv + optind;
  setup_fds();
  init_forkserver(use_argv);
  read_testcases();

  if (report_file) {
    report_fd = open(report_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (report_fd < 0) PFATAL("Unable to open '%s'", report_file);
  }

  if (map_file) {
    fd = open(map_file, O_RDONLY);
    if (fd > 0) {
      if (read(fd, virgin_bits, MAP_SIZE) != MAP_SIZE) FATAL("Short read from '%s'", map_file);
      if (read(fd, virgin_crash, MAP_SIZE) != MAP_SIZE) FATAL("Short read from '%s'", map_file);
    }
    close(fd);
  }

  for (u32 i = 0; i < qu.size; i+= 1) {
    u8* use_mem;
    queue_entry q = qu.entries[i];

    fd = open(q.fname, O_RDONLY);
    if (fd < 0) PFATAL("Unable to open '%s'", q.fname);
    use_mem = ck_alloc_nozero(q.len);
    if (read(fd, use_mem, q.len) != q.len) FATAL("Short read from '%s'", q.fname);
    close(fd);
    write_to_testcase(use_mem, q.len);
    ck_free(use_mem);

    // Update stats
    u8 res = run_target(use_argv, exec_tmout);
    u8 hnb_normal = has_new_bits(virgin_bits);
    u8 hnb_crash = res == 2 ? has_new_bits(virgin_crash) : 0;

    if (i % 1000 == 0) {
      OKF("%d/%d Reading %s", i, qu.size, q.fname);
    }

    if (hnb_normal || hnb_crash) {
      u8* line = alloc_printf("%d:%d:%s\n", res, hnb_normal, q.fname);
      write(report_fd, line, strlen(line));
      ck_free(line);
    }
    // Write to trace_dir
    if (trace_dir && (hnb_normal || hnb_crash)) {
      u8* fname = alloc_printf("%s/%s", trace_dir, basename(q.fname));
      fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0) PFATAL("Unable to open '%s'", fname);
      ck_write(fd, trace_bits, MAP_SIZE, fname);
      close(fd);
      ck_free(fname);
    }
    // Write to trace_dir
    if (branch_dir && (hnb_normal || hnb_crash)) {
      u8* fname = alloc_printf("%s/%s", branch_dir, basename(q.fname));
      fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0) PFATAL("Unable to open '%s'", fname);
      ck_write(fd, trace_bits + MAP_SIZE, MAP_SIZE * 8, fname);
      close(fd);
      ck_free(fname);
    }
  }

  if (map_file) {
    fd = open(map_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) PFATAL("Unable to open '%s'", map_file);
    if (write(fd, virgin_bits, MAP_SIZE) != MAP_SIZE) PFATAL("Unable to write '%s'", map_file);
    if (write(fd, virgin_crash, MAP_SIZE) != MAP_SIZE) PFATAL("Unable to write '%s'", map_file);
    close(fd);
  }
  if (report_file) close(report_fd);
  return 0;
}
