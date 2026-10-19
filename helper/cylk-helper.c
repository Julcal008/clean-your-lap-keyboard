/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * cylk-helper: minimal privileged helper for Clean Your Lap Keyboard.
 *
 * It receives a single GRAB command over stdin, opens the requested evdev
 * device, takes an exclusive grab, drops privileges to the invoking user and
 * then only waits for RELEASE, EOF, the emergency Escape hold or the maximum
 * duration.  It never reads the evdev event stream and never logs key data.
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EVENT_PREFIX "/dev/input/event"
#define MAX_LINE 512
#define KEY_WORDS (KEY_CNT / (8 * sizeof(unsigned long)) + 1)

static volatile sig_atomic_t stop_requested = 0;

static void
on_signal(int signo)
{
  (void) signo;
  stop_requested = 1;
}

static void
install_signal_handlers(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
}

static int64_t
now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
send_error(const char *code, const char *fmt, ...)
{
  va_list ap;

  printf("ERR\t%s\t", code);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}

static void
send_released(const char *reason)
{
  printf("RELEASED\t%s\n", reason);
  fflush(stdout);
}

static void
release_and_exit(int fd, const char *reason)
{
  if (fd >= 0) {
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
  }
  send_released(reason);
  exit(0);
}

static bool
test_bit(const unsigned long *bits, unsigned int bit)
{
  const unsigned int word_bits = 8 * sizeof(unsigned long);
  return (bits[bit / word_bits] >> (bit % word_bits)) & 1UL;
}

static bool
is_full_keyboard(int fd)
{
  unsigned long evbits[(EV_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };
  unsigned long keybits[KEY_WORDS] = { 0 };

  if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
    return false;

  if (!test_bit(evbits, EV_KEY))
    return false;

  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
    return false;

  return test_bit(keybits, KEY_A) &&
         test_bit(keybits, KEY_Z) &&
         test_bit(keybits, KEY_0) &&
         test_bit(keybits, KEY_ENTER) &&
         test_bit(keybits, KEY_SPACE) &&
         test_bit(keybits, KEY_LEFTSHIFT);
}

static bool
any_key_down(int fd)
{
  unsigned long keybits[KEY_WORDS] = { 0 };

  if (ioctl(fd, EVIOCGKEY(sizeof keybits), keybits) < 0)
    return false;

  for (unsigned int i = 0; i < KEY_CNT; i++) {
    if (test_bit(keybits, i))
      return true;
  }
  return false;
}

static bool
escape_down(int fd)
{
  unsigned long keybits[KEY_WORDS] = { 0 };

  if (ioctl(fd, EVIOCGKEY(sizeof keybits), keybits) < 0)
    return false;

  return test_bit(keybits, KEY_ESC);
}

static bool
valid_event_path(const char *path, char *resolved, size_t resolved_size)
{
  const size_t prefix_len = strlen(EVENT_PREFIX);
  struct stat st;
  char real[PATH_MAX];

  if (path == NULL || strncmp(path, EVENT_PREFIX, prefix_len) != 0)
    return false;

  const char *suffix = path + prefix_len;
  if (*suffix == '\0')
    return false;

  for (const char *p = suffix; *p != '\0'; p++) {
    if (*p < '0' || *p > '9')
      return false;
  }

  if (realpath(path, real) == NULL)
    return false;

  if (strncmp(real, EVENT_PREFIX, prefix_len) != 0)
    return false;

  if (stat(real, &st) < 0 || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 13)
    return false;

  snprintf(resolved, resolved_size, "%s", real);
  return true;
}

static bool
drop_privileges(void)
{
  const char *uid_env = getenv("PKEXEC_UID");
  struct passwd *pw;
  uid_t uid;

  if (uid_env == NULL || *uid_env == '\0') {
    fprintf(stderr, "cylk-helper: PKEXEC_UID not set; keeping current uid\n");
    return true;
  }

  uid = (uid_t) strtoul(uid_env, NULL, 10);
  if (uid == 0)
    return true;

  pw = getpwuid(uid);
  if (pw == NULL) {
    fprintf(stderr, "cylk-helper: no passwd entry for uid %u\n", (unsigned) uid);
    return false;
  }

  if (setgroups(0, NULL) != 0) {
    fprintf(stderr, "cylk-helper: setgroups: %s\n", strerror(errno));
    return false;
  }
  if (setgid(pw->pw_gid) != 0) {
    fprintf(stderr, "cylk-helper: setgid: %s\n", strerror(errno));
    return false;
  }
  if (setuid(uid) != 0) {
    fprintf(stderr, "cylk-helper: setuid: %s\n", strerror(errno));
    return false;
  }

  (void) prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  (void) prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

  return true;
}

static void
sanitize_name(char *name)
{
  for (char *p = name; *p != '\0'; p++) {
    if (*p == '\t' || *p == '\n' || *p == '\r')
      *p = ' ';
  }
}

static int
wait_for_release_of_keys(int fd)
{
  int64_t deadline = now_ms() + 2000;

  while (now_ms() < deadline) {
    if (!any_key_down(fd))
      return 0;
    usleep(50000);
  }
  return -1;
}

int
main(void)
{
  char line[MAX_LINE];
  char path[PATH_MAX] = { 0 };
  int fd = -1;
  unsigned int hold_ms = 2000;
  unsigned int max_s = 65;
  int64_t grab_started;
  int64_t escape_since = 0;
  int64_t handshake_deadline;
  bool grabbed = false;

  setvbuf(stdout, NULL, _IOLBF, 0);
  umask(077);
  install_signal_handlers();
  (void) prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  (void) prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

  if (getppid() == 1)
    return 1;

  printf("READY\n");
  fflush(stdout);

  handshake_deadline = now_ms() + 15000;

  while (!grabbed) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int ret;

    if (stop_requested)
      return 0;

    if (now_ms() >= handshake_deadline) {
      send_error("timeout", "No GRAB command received");
      return 1;
    }

    ret = poll(&pfd, 1, 200);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      send_error("poll", "%s", strerror(errno));
      return 1;
    }
    if (ret == 0)
      continue;

    if (fgets(line, sizeof line, stdin) == NULL) {
      /* The GUI went away before asking us to grab anything. */
      return 0;
    }

    line[strcspn(line, "\r\n")] = '\0';

    if (strcmp(line, "QUIT") == 0)
      return 0;

    if (strncmp(line, "GRAB\t", 5) != 0) {
      send_error("protocol", "Expected GRAB, RELEASE or QUIT");
      return 1;
    }

    if (sscanf(line + 5, "%4095s\t%u\t%u", path, &hold_ms, &max_s) != 3) {
      send_error("protocol", "Malformed GRAB command");
      return 1;
    }

    if (hold_ms < 500 || hold_ms > 5000) {
      send_error("invalid-hold", "Escape hold must be between 0.5 and 5 seconds");
      return 1;
    }
    if (max_s < 5 || max_s > 600) {
      send_error("invalid-duration", "Duration must be between 5 and 600 seconds");
      return 1;
    }

    char resolved[PATH_MAX];
    if (!valid_event_path(path, resolved, sizeof resolved)) {
      send_error("invalid-path", "Not an evdev event device: %s", path);
      return 1;
    }
    snprintf(path, sizeof path, "%s", resolved);

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      send_error("open", "Cannot open %s: %s", path, strerror(errno));
      return 1;
    }

    char name[256] = { 0 };
    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0) {
      send_error("name", "Cannot read device name: %s", strerror(errno));
      close(fd);
      return 1;
    }
    sanitize_name(name);

    if (!is_full_keyboard(fd)) {
      send_error("not-keyboard", "%s is not a full keyboard", name);
      close(fd);
      return 1;
    }

    if (wait_for_release_of_keys(fd) != 0) {
      send_error("keys-down", "Release all keys and try again");
      close(fd);
      return 1;
    }

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
      send_error("grab", "Cannot grab %s: %s%s", name, strerror(errno),
                 errno == EBUSY ? " (already grabbed by another application)" : "");
      close(fd);
      return 1;
    }

    grabbed = true;

    if (!drop_privileges()) {
      ioctl(fd, EVIOCGRAB, 0);
      close(fd);
      send_error("privileges", "Could not drop privileges");
      return 1;
    }

    (void) prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

    printf("OK\t%s\n", name);
    fflush(stdout);
    grab_started = now_ms();
  }

  /*
   * From here on we own the keyboard.  Keep the polling loop extremely
   * small: watch stdin for RELEASE/QUIT, poll the Escape key state and
   * enforce the maximum duration.
   */
  while (true) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int ret;

    if (stop_requested)
      release_and_exit(fd, "signal");

    ret = poll(&pfd, 1, 50);
    if (ret < 0) {
      if (errno != EINTR)
        release_and_exit(fd, "poll-error");
    } else if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
      if (fgets(line, sizeof line, stdin) == NULL)
        release_and_exit(fd, "disconnected");

      line[strcspn(line, "\r\n")] = '\0';
      if (strcmp(line, "RELEASE") == 0 || strcmp(line, "QUIT") == 0)
        release_and_exit(fd, "user");
    }

    if (escape_down(fd)) {
      if (escape_since == 0)
        escape_since = now_ms();
      else if (now_ms() - escape_since >= (int64_t) hold_ms)
        release_and_exit(fd, "escape");
    } else {
      escape_since = 0;
    }

    if (now_ms() - grab_started >= (int64_t) max_s * 1000)
      release_and_exit(fd, "timeout");
  }
}
