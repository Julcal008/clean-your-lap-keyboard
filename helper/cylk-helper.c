/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * cylk-helper: minimal privileged helper for Clean Your Lap Keyboard.
 *
 * It receives a single GRAB command over stdin with the main keyboard device,
 * an optional list of companion hotkey devices (volume, brightness, power
 * button, ...) and an optional internal touchpad.  It opens every device, takes
 * an exclusive grab on all of them, drops privileges to the invoking user and
 * then only waits for RELEASE, EOF, the emergency Escape hold or the maximum
 * duration.  It never reads the evdev event stream and never logs key data.
 */

#include "src/companion-keys.h"

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
#define MAX_LINE 2048
#define MAX_COMPANIONS 8
#define MAX_TOUCHPADS 4
#define KEY_WORDS (KEY_CNT / (8 * sizeof(unsigned long)) + 1)

typedef struct {
  int fd;
  char path[PATH_MAX];
  char name[256];
  bool touchpad;
} grab_target;

static grab_target targets[MAX_COMPANIONS + MAX_TOUCHPADS + 1];
static size_t n_targets = 0;
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
close_all(void)
{
  for (size_t i = 0; i < n_targets; i++) {
    if (targets[i].fd >= 0) {
      close(targets[i].fd);
      targets[i].fd = -1;
    }
  }
}

static void
release_all(const char *reason)
{
  for (size_t i = n_targets; i > 0; i--) {
    grab_target *target = &targets[i - 1];

    if (target->fd >= 0) {
      ioctl(target->fd, EVIOCGRAB, 0);
      close(target->fd);
      target->fd = -1;
    }
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
has_any_bit(const unsigned long *bits, const unsigned int *keys, size_t n_keys)
{
  for (size_t i = 0; i < n_keys; i++) {
    if (test_bit(bits, keys[i]))
      return true;
  }
  return false;
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

/*
 * Decide whether an already-open fd may be grabbed as a companion device.
 * It must be a key device, must not be a pointer and must not be a second
 * keyboard; it must also expose at least one hotkey or button key.  This
 * mirrors the classification done in device.c but re-checks it on the real
 * device, because the helper must not trust the caller blindly.
 */
static bool
is_companion_device(int fd)
{
  unsigned long evbits[(EV_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };
  unsigned long keybits[KEY_WORDS] = { 0 };
  unsigned long relbits[(REL_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };
  unsigned long absbits[(ABS_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };

  if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
    return false;

  if (!test_bit(evbits, EV_KEY))
    return false;

  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
    return false;

  /* Never grab a pointer. */
  if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits) >= 0) {
    if (test_bit(relbits, REL_X) || test_bit(relbits, REL_Y))
      return false;
  }
  if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) >= 0) {
    if (test_bit(absbits, ABS_X) || test_bit(absbits, ABS_Y))
      return false;
  }

  /* A second keyboard is not a companion device. */
  if (is_full_keyboard(fd))
    return false;

  return has_any_bit(keybits, cylk_companion_keys,
                     sizeof cylk_companion_keys / sizeof cylk_companion_keys[0]) ||
         has_any_bit(keybits, cylk_button_keys,
                     sizeof cylk_button_keys / sizeof cylk_button_keys[0]);
}

/*
 * Decide whether an already-open fd may be grabbed as the built-in touchpad.
 * It must be a pointer (relative or absolute axes), must expose touch
 * capabilities and must not be a full keyboard.  A plain mouse is rejected
 * because it has no touch keys.
 */
static bool
is_touchpad_device(int fd)
{
  static const unsigned int touch_keys[] = {
    BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
  };
  static const unsigned int multitouch_axes[] = {
    ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
  };
  unsigned long evbits[(EV_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };
  unsigned long keybits[KEY_WORDS] = { 0 };
  unsigned long relbits[(REL_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };
  unsigned long absbits[(ABS_MAX / (8 * sizeof(unsigned long))) + 1] = { 0 };
  bool has_axes = false;

  if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
    return false;

  if (!test_bit(evbits, EV_KEY))
    return false;

  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
    return false;

  if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits) >= 0) {
    if (test_bit(relbits, REL_X) || test_bit(relbits, REL_Y))
      has_axes = true;
  }
  if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) >= 0) {
    if (test_bit(absbits, ABS_X) || test_bit(absbits, ABS_Y) ||
        test_bit(absbits, ABS_MT_POSITION_X) ||
        test_bit(absbits, ABS_MT_POSITION_Y))
      has_axes = true;
  }

  if (!has_axes)
    return false;

  if (is_full_keyboard(fd))
    return false;

  return has_any_bit(keybits, touch_keys,
                     sizeof touch_keys / sizeof touch_keys[0]) ||
         has_any_bit(absbits, multitouch_axes,
                     sizeof multitouch_axes / sizeof multitouch_axes[0]);
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
open_target(const char *path, bool primary, bool touchpad, char *error,
            size_t error_size)
{
  char resolved[PATH_MAX];
  char name[256] = { 0 };
  int fd;

  if (!valid_event_path(path, resolved, sizeof resolved)) {
    snprintf(error, error_size, "Not an evdev event device: %s", path);
    return -1;
  }

  for (size_t i = 0; i < n_targets; i++) {
    if (strcmp(targets[i].path, resolved) == 0) {
      snprintf(error, error_size, "Duplicate device: %s", resolved);
      return -1;
    }
  }

  fd = open(resolved, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    snprintf(error, error_size, "Cannot open %s: %s", resolved, strerror(errno));
    return -1;
  }

  if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0) {
    snprintf(error, error_size, "Cannot read device name: %s", strerror(errno));
    close(fd);
    return -1;
  }
  sanitize_name(name);

  if (primary ? !is_full_keyboard(fd)
              : touchpad ? !is_touchpad_device(fd)
                         : !is_companion_device(fd)) {
    snprintf(error, error_size, "%s is not %s", name,
             primary    ? "a full keyboard"
             : touchpad ? "a touchpad"
                        : "a supported hotkey device");
    close(fd);
    return -1;
  }

  targets[n_targets].fd = fd;
  targets[n_targets].touchpad = touchpad;
  snprintf(targets[n_targets].path, sizeof targets[n_targets].path, "%s",
           resolved);
  snprintf(targets[n_targets].name, sizeof targets[n_targets].name, "%s", name);
  n_targets++;
  return 0;
}

static int
wait_for_release_of_keys(void)
{
  int64_t deadline = now_ms() + 2000;

  while (now_ms() < deadline) {
    bool down = false;

    for (size_t i = 0; i < n_targets; i++) {
      /* A finger resting on the touchpad is not a pressed key. */
      if (targets[i].touchpad)
        continue;

      if (any_key_down(targets[i].fd)) {
        down = true;
        break;
      }
    }

    if (!down)
      return 0;

    usleep(50000);
  }
  return -1;
}

int
main(void)
{
  char line[MAX_LINE];
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

  for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++)
    targets[i].fd = -1;

  if (getppid() == 1)
    return 1;

  printf("READY\t3\n");
  fflush(stdout);

  handshake_deadline = now_ms() + 15000;

  while (!grabbed) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    char primary[PATH_MAX];
    char extras[MAX_COMPANIONS][PATH_MAX];
    char touchpads[MAX_TOUCHPADS][PATH_MAX];
    size_t n_extras = 0;
    size_t n_touchpads = 0;
    char *saveptr = NULL;
    char *token;
    char error[PATH_MAX + 128];
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

    token = strtok_r(line + 5, "\t", &saveptr);
    if (token == NULL) {
      send_error("protocol", "Malformed GRAB command");
      return 1;
    }
    snprintf(primary, sizeof primary, "%s", token);

    token = strtok_r(NULL, "\t", &saveptr);
    if (token == NULL || sscanf(token, "%u", &hold_ms) != 1) {
      send_error("protocol", "Malformed GRAB command");
      return 1;
    }

    token = strtok_r(NULL, "\t", &saveptr);
    if (token == NULL || sscanf(token, "%u", &max_s) != 1) {
      send_error("protocol", "Malformed GRAB command");
      return 1;
    }

    while ((token = strtok_r(NULL, "\t", &saveptr)) != NULL) {
      if (*token == '\0')
        continue;

      if (strncmp(token, "T:", 2) == 0) {
        if (token[2] == '\0') {
          send_error("protocol", "Malformed touchpad device");
          return 1;
        }
        if (n_touchpads >= MAX_TOUCHPADS) {
          send_error("protocol", "Too many touchpad devices");
          return 1;
        }
        snprintf(touchpads[n_touchpads], PATH_MAX, "%s", token + 2);
        n_touchpads++;
        continue;
      }

      if (n_extras >= MAX_COMPANIONS) {
        send_error("protocol", "Too many companion devices");
        return 1;
      }
      snprintf(extras[n_extras], PATH_MAX, "%s", token);
      n_extras++;
    }

    if (hold_ms < 500 || hold_ms > 5000) {
      send_error("invalid-hold", "Escape hold must be between 0.5 and 5 seconds");
      return 1;
    }
    if (max_s < 5 || max_s > 600) {
      send_error("invalid-duration", "Duration must be between 5 and 600 seconds");
      return 1;
    }

    n_targets = 0;

    if (open_target(primary, true, false, error, sizeof error) != 0) {
      send_error("not-keyboard", "%s", error);
      close_all();
      return 1;
    }

    for (size_t i = 0; i < n_extras; i++) {
      if (open_target(extras[i], false, false, error, sizeof error) != 0) {
        send_error("device", "%s", error);
        close_all();
        return 1;
      }
    }

    for (size_t i = 0; i < n_touchpads; i++) {
      if (open_target(touchpads[i], false, true, error, sizeof error) != 0) {
        send_error("device", "%s", error);
        close_all();
        return 1;
      }
    }

    if (wait_for_release_of_keys() != 0) {
      send_error("keys-down", "Release all keys and try again");
      close_all();
      return 1;
    }

    for (size_t i = 0; i < n_targets; i++) {
      if (ioctl(targets[i].fd, EVIOCGRAB, 1) < 0) {
        for (size_t j = 0; j < i; j++)
          ioctl(targets[j].fd, EVIOCGRAB, 0);

        send_error("grab", "Cannot grab %s: %s%s", targets[i].name,
                   strerror(errno),
                   errno == EBUSY
                       ? " (already grabbed by another application)"
                       : "");
        close_all();
        return 1;
      }
    }

    grabbed = true;

    if (!drop_privileges()) {
      for (size_t i = 0; i < n_targets; i++)
        ioctl(targets[i].fd, EVIOCGRAB, 0);
      close_all();
      send_error("privileges", "Could not drop privileges");
      return 1;
    }

    (void) prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

    printf("OK\t%s\n", targets[0].name);
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
      release_all("signal");

    ret = poll(&pfd, 1, 50);
    if (ret < 0) {
      if (errno != EINTR)
        release_all("poll-error");
    } else if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
      if (fgets(line, sizeof line, stdin) == NULL)
        release_all("disconnected");

      line[strcspn(line, "\r\n")] = '\0';
      if (strcmp(line, "RELEASE") == 0 || strcmp(line, "QUIT") == 0)
        release_all("user");
    }

    if (escape_down(targets[0].fd)) {
      if (escape_since == 0)
        escape_since = now_ms();
      else if (now_ms() - escape_since >= (int64_t) hold_ms)
        release_all("escape");
    } else {
      escape_since = 0;
    }

    if (now_ms() - grab_started >= (int64_t) max_s * 1000)
      release_all("timeout");
  }
}
