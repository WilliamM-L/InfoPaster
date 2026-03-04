/* info_paster — C99 TUI clipboard tool
 *
 * Reads personal info from a flat JSON file and presents it in a TUI.
 * User presses a 1-2 char shortcut key to instantly copy a value to clipboard.
 *
 * Build: ./build.sh
 * Run:   ./bin/info_paster [path/to/info.json]
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* ───────────────────────────── Arena Allocator ─────────────────────────────
 */

#define ARENA_SIZE (1 << 20) /* 1 MiB */

typedef struct {
  unsigned char *buf;
  size_t cap;
  size_t used;
} Arena;

static int arena_init(Arena *a, size_t cap) {
  a->buf = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
  if (a->buf == MAP_FAILED)
    return -1;
  a->cap = cap;
  a->used = 0;
  return 0;
}

static void *arena_alloc(Arena *a, size_t size) {
  /* align to 8 bytes */
  size = (size + 7) & ~(size_t)7;
  if (a->used + size > a->cap) {
    fprintf(stderr, "arena: out of memory\n");
    return NULL;
  }
  void *ptr = a->buf + a->used;
  a->used += size;
  return ptr;
}

static void arena_destroy(Arena *a) {
  if (a->buf && a->buf != MAP_FAILED) {
    munmap(a->buf, a->cap);
  }
  a->buf = NULL;
  a->cap = 0;
  a->used = 0;
}

/* ──────────────────────────── Data Structures ──────────────────────────────
 */

#define MAX_ENTRIES 64

typedef struct {
  char *label;
  char *value;
  char shortcut[3]; /* 1-2 chars + NUL */
} InfoEntry;

typedef struct {
  InfoEntry entries[MAX_ENTRIES];
  int count;
} InfoStore;

/* ──────────────────────── Minimal JSON Parser ──────────────────────────────
 */
/*
 * Only supports the flat { "key": "value", ... } schema.
 * No nested objects, arrays, numbers, booleans, or null.
 * String escapes handled: \", \\, \/, \n, \t, \r, \b, \f.
 */

static void skip_ws(const char **p) {
  while (**p && isspace((unsigned char)**p))
    (*p)++;
}

/* Parse a JSON string starting at the opening '"'. Returns length via *out_len.
 * Returns pointer to start of string content (past the quote). */
static const char *parse_json_string(const char **p, size_t *out_len) {
  if (**p != '"')
    return NULL;
  (*p)++; /* skip opening quote */
  const char *start = *p;

  /* first pass: find end and compute decoded length */
  size_t len = 0;
  const char *scan = start;
  while (*scan && *scan != '"') {
    if (*scan == '\\') {
      scan++; /* skip escape char */
      if (!*scan)
        return NULL;
    }
    len++;
    scan++;
  }
  if (*scan != '"')
    return NULL;

  *out_len = len;
  /* advance past closing quote */
  *p = scan + 1;
  return start;
}

/* Decode a JSON string in-place into arena memory */
static char *decode_json_string(Arena *a, const char *src, size_t raw_len) {
  /* Worst case: raw_len chars (escapes shrink) */
  char *out = arena_alloc(a, raw_len + 1);
  if (!out)
    return NULL;

  size_t j = 0;
  for (size_t i = 0; i < raw_len && src[i]; i++) {
    if (src[i] == '\\' && i + 1 < raw_len) {
      i++;
      switch (src[i]) {
      case '"':
        out[j++] = '"';
        break;
      case '\\':
        out[j++] = '\\';
        break;
      case '/':
        out[j++] = '/';
        break;
      case 'n':
        out[j++] = '\n';
        break;
      case 't':
        out[j++] = '\t';
        break;
      case 'r':
        out[j++] = '\r';
        break;
      case 'b':
        out[j++] = '\b';
        break;
      case 'f':
        out[j++] = '\f';
        break;
      default:
        out[j++] = src[i];
        break;
      }
    } else if (src[i] == '"') {
      break; /* end of string */
    } else {
      out[j++] = src[i];
    }
  }
  out[j] = '\0';
  return out;
}

static int parse_json(const char *json, Arena *a, InfoStore *store) {
  const char *p = json;
  store->count = 0;

  skip_ws(&p);
  if (*p != '{') {
    fprintf(stderr, "json: expected '{'\n");
    return -1;
  }
  p++;

  while (1) {
    skip_ws(&p);
    if (*p == '}')
      break;

    /* expect comma between entries */
    if (store->count > 0) {
      if (*p != ',') {
        fprintf(stderr, "json: expected ',' at offset %ld\n", (long)(p - json));
        return -1;
      }
      p++;
      skip_ws(&p);
    }

    if (*p == '}')
      break; /* trailing comma tolerance */

    /* parse key */
    size_t key_len;
    const char *key_raw = parse_json_string(&p, &key_len);
    if (!key_raw) {
      fprintf(stderr, "json: expected string key at offset %ld\n",
              (long)(p - json));
      return -1;
    }

    skip_ws(&p);
    if (*p != ':') {
      fprintf(stderr, "json: expected ':' at offset %ld\n", (long)(p - json));
      return -1;
    }
    p++;
    skip_ws(&p);

    /* parse value */
    size_t val_len;
    const char *val_raw = parse_json_string(&p, &val_len);
    if (!val_raw) {
      fprintf(stderr, "json: expected string value at offset %ld\n",
              (long)(p - json));
      return -1;
    }

    if (store->count >= MAX_ENTRIES) {
      fprintf(stderr, "json: too many entries (max %d)\n", MAX_ENTRIES);
      return -1;
    }

    /* compute raw_len = distance from raw pointer to closing quote */
    size_t key_raw_len = (size_t)(val_raw - 1 - key_raw);
    /* actually, we need to recompute: raw pointer to where the closing
     * quote was. Let's just use the scan-based length. */
    (void)key_raw_len;

    InfoEntry *e = &store->entries[store->count];
    e->label = decode_json_string(a, key_raw, key_len);
    e->value = decode_json_string(a, val_raw, val_len);
    if (!e->label || !e->value)
      return -1;

    store->count++;
  }

  return 0;
}

/* ──────────────────────── Shortcut Key Assignment ──────────────────────────
 */

/*
 * Derives shortcut keys from labels:
 *   - First letter of the label, lowercased (e.g., 'e' for "Email")
 *   - On collision, uses first two lowercase letters (e.g., "fu" vs "fo")
 *   - 'q' is reserved for quit — skipped to next candidate
 */
static int shortcut_taken(const InfoStore *store, int up_to, const char *sc) {
  for (int j = 0; j < up_to; j++) {
    if (strcmp(store->entries[j].shortcut, sc) == 0)
      return 1;
  }
  return 0;
}

static void assign_shortcuts(InfoStore *store) {
  for (int i = 0; i < store->count && i < MAX_ENTRIES; i++) {
    const char *label = store->entries[i].label;
    char candidate[3] = {0};

    /* Try single first letter */
    char first = (char)tolower((unsigned char)label[0]);
    if (first && first != 'q') {
      candidate[0] = first;
      candidate[1] = '\0';
      if (!shortcut_taken(store, i, candidate)) {
        memcpy(store->entries[i].shortcut, candidate, 3);
        continue;
      }
    }

    /* Collision: try first letter + next lowercase alpha from label */
    int found = 0;
    for (int k = 1; label[k] && !found; k++) {
      if (!isalpha((unsigned char)label[k]))
        continue;
      char second = (char)tolower((unsigned char)label[k]);
      candidate[0] = first ? first : '?';
      candidate[1] = second;
      candidate[2] = '\0';
      if (!shortcut_taken(store, i, candidate)) {
        memcpy(store->entries[i].shortcut, candidate, 3);
        found = 1;
      }
    }

    /* Last resort: use a digit */
    if (!found) {
      for (char d = '1'; d <= '9'; d++) {
        candidate[0] = d;
        candidate[1] = '\0';
        if (!shortcut_taken(store, i, candidate)) {
          memcpy(store->entries[i].shortcut, candidate, 3);
          found = 1;
          break;
        }
      }
    }
  }
}

/* ──────────────────────────── Clipboard ────────────────────────────────────
 */

static int copy_to_clipboard(const char *text) {
  /* Try xclip first, then xsel, then wl-copy */
  const char *cmds[] = {
      "xclip -selection clipboard",
      "xsel --clipboard --input",
      "wl-copy",
  };
  int n = (int)(sizeof(cmds) / sizeof(cmds[0]));

  for (int i = 0; i < n; i++) {
    FILE *fp = popen(cmds[i], "w");
    if (fp) {
      size_t len = strlen(text);
      size_t written = fwrite(text, 1, len, fp);
      int status = pclose(fp);
      if (status == 0 && written == len)
        return 0;
    }
  }
  return -1;
}

/* ────────────────────────── Terminal / TUI ──────────────────────────────────
 */

static struct termios g_orig_termios;
static int g_raw_mode = 0;

static void disable_raw_mode(void) {
  if (g_raw_mode) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_raw_mode = 0;
  }
}

static int enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1)
    return -1;
  atexit(disable_raw_mode);

  struct termios raw = g_orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG);
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    return -1;
  g_raw_mode = 1;
  return 0;
}

/* ANSI helpers */
#define ESC "\033["
#define CLEAR ESC "2J"
#define HOME ESC "H"
#define BOLD ESC "1m"
#define DIM ESC "2m"
#define RESET ESC "0m"
#define FG_CYAN ESC "36m"
#define FG_GREEN ESC "32m"
#define FG_YELLOW ESC "33m"
#define FG_WHITE ESC "97m"
#define BG_GRAY ESC "48;5;236m"
#define HIDE_CUR ESC "?25l"
#define SHOW_CUR ESC "?25h"

static void get_terminal_width(int *width) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    *width = ws.ws_col;
  } else {
    *width = 60;
  }
}

static void draw_tui(const InfoStore *store, int copied_idx) {
  int tw;
  get_terminal_width(&tw);
  if (tw > 120)
    tw = 120;

  /* find max label width for alignment */
  int max_label = 0;
  for (int i = 0; i < store->count; i++) {
    int len = (int)strlen(store->entries[i].label);
    if (len > max_label)
      max_label = len;
  }

  /* find max shortcut width */
  int sc_width = 1;
  for (int i = 0; i < store->count; i++) {
    int len = (int)strlen(store->entries[i].shortcut);
    if (len > sc_width)
      sc_width = len;
  }

  printf(CLEAR HOME HIDE_CUR);

  /* header */
  printf("\n");
  printf("  " BOLD FG_CYAN "INFO PASTER" RESET "\n");
  printf("  " DIM "Press a key to copy to clipboard. 'q' to quit." RESET "\n");
  printf("\n");

  /* separator */
  printf("  " DIM);
  for (int i = 0; i < tw - 4 && i < max_label + sc_width + 8; i++)
    printf("─");
  printf(RESET "\n\n");

  /* entries */
  for (int i = 0; i < store->count; i++) {
    const InfoEntry *e = &store->entries[i];

    if (i == copied_idx) {
      printf("  " BOLD FG_GREEN "[%*s]" RESET "  ", sc_width, e->shortcut);
      printf(BOLD FG_GREEN "%-*s" RESET, max_label, e->label);
      printf("   " FG_GREEN "✓ Copied!" RESET "\n");
    } else {
      printf("  " FG_YELLOW "[%*s]" RESET "  ", sc_width, e->shortcut);
      printf(FG_WHITE "%-*s" RESET, max_label, e->label);
      printf("   " DIM "%s" RESET "\n", e->value);
    }
  }

  /* footer */
  printf("\n");
  printf("  " DIM);
  for (int i = 0; i < tw - 4 && i < max_label + sc_width + 8; i++)
    printf("─");
  printf(RESET "\n");

  fflush(stdout);
}

/* ──────────────────────── Input Matching ───────────────────────────────────
 */

/* Check if any shortcut starts with the given prefix */
static int any_shortcut_starts_with(const InfoStore *store,
                                    const char *prefix) {
  size_t plen = strlen(prefix);
  for (int i = 0; i < store->count; i++) {
    if (strncmp(store->entries[i].shortcut, prefix, plen) == 0 &&
        strlen(store->entries[i].shortcut) > plen) {
      return 1;
    }
  }
  return 0;
}

/* Read a single byte with optional timeout.
 * timeout_ms <= 0 means block indefinitely.
 * Returns 1 on success, 0 on timeout, -1 on error/EOF.
 */
static int read_byte(char *out, int timeout_ms) {
  if (timeout_ms > 0) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0)
      return ret == 0 ? 0 : -1;
  }
  ssize_t n = read(STDIN_FILENO, out, 1);
  return n <= 0 ? -1 : 1;
}

/* Find entry matching exact shortcut */
static int find_shortcut(const InfoStore *store, const char *key) {
  for (int i = 0; i < store->count; i++) {
    if (strcmp(store->entries[i].shortcut, key) == 0)
      return i;
  }
  return -1;
}

/* ────────────────────────────── File I/O ───────────────────────────────────
 */

static char *read_file(Arena *a, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "Cannot open '%s': %s\n", path, strerror(errno));
    return NULL;
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0) {
    fprintf(stderr, "Cannot stat '%s'\n", path);
    close(fd);
    return NULL;
  }

  char *buf = arena_alloc(a, (size_t)st.st_size + 1);
  if (!buf) {
    close(fd);
    return NULL;
  }

  ssize_t n = read(fd, buf, (size_t)st.st_size);
  close(fd);
  if (n < 0) {
    fprintf(stderr, "Cannot read '%s': %s\n", path, strerror(errno));
    return NULL;
  }
  buf[n] = '\0';
  return buf;
}

/* ──────────────────────────────── Main ─────────────────────────────────────
 */

int main(int argc, char **argv) {
  const char *json_path = (argc > 1) ? argv[1] : "info.json";

  Arena arena;
  if (arena_init(&arena, ARENA_SIZE) < 0) {
    fprintf(stderr, "Failed to allocate arena\n");
    return 1;
  }

  char *json_text = read_file(&arena, json_path);
  if (!json_text) {
    arena_destroy(&arena);
    return 1;
  }

  InfoStore store;
  if (parse_json(json_text, &arena, &store) < 0) {
    arena_destroy(&arena);
    return 1;
  }

  if (store.count == 0) {
    fprintf(stderr, "No entries found in '%s'\n", json_path);
    arena_destroy(&arena);
    return 1;
  }

  assign_shortcuts(&store);

  if (enable_raw_mode() < 0) {
    fprintf(stderr, "Failed to enable raw mode\n");
    arena_destroy(&arena);
    return 1;
  }

  int copied_idx = -1;
  char input_buf[3] = {0};
  int input_len = 0;
  int timeout_ms = 0; /* 0 = block indefinitely */

  draw_tui(&store, copied_idx);

  while (1) {
    char c;
    int rr = read_byte(&c, timeout_ms);
    timeout_ms = 0;

    if (rr < 0)
      break;

    if (rr == 0) {
      /* timeout: fire the best match for current input */
      int match = find_shortcut(&store, input_buf);
      if (match >= 0) {
        if (copy_to_clipboard(store.entries[match].value) == 0)
          copied_idx = match;
        else
          copied_idx = -1;
      } else {
        copied_idx = -1;
      }
      input_len = 0;
      input_buf[0] = '\0';
      draw_tui(&store, copied_idx);
      continue;
    }

    /* quit */
    if (c == 'q' && input_len == 0) {
      break;
    }

    /* escape: clear any partial input */
    if (c == 27) {
      input_len = 0;
      input_buf[0] = '\0';
      copied_idx = -1;
      draw_tui(&store, copied_idx);
      continue;
    }

    /* accumulate input */
    if (input_len < 2) {
      input_buf[input_len++] = c;
      input_buf[input_len] = '\0';
    }

    /* check if any longer shortcut starts with current input */
    int has_longer = any_shortcut_starts_with(&store, input_buf);

    /* check for exact match */
    int match = find_shortcut(&store, input_buf);

    if (match >= 0 && !has_longer) {
      /* unambiguous exact match — copy to clipboard */
      if (copy_to_clipboard(store.entries[match].value) == 0) {
        copied_idx = match;
      } else {
        copied_idx = -1;
      }
      input_len = 0;
      input_buf[0] = '\0';
      draw_tui(&store, copied_idx);
      continue;
    }

    if (has_longer) {
      timeout_ms = 500; /* wait up to 500ms for next char */
      continue;
    }

    /* no match possible: reset */
    input_len = 0;
    input_buf[0] = '\0';
    copied_idx = -1;
    draw_tui(&store, copied_idx);
  }

  /* restore terminal */
  printf(SHOW_CUR CLEAR HOME);
  fflush(stdout);
  disable_raw_mode();
  arena_destroy(&arena);

  return 0;
}
