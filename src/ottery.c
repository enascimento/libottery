#define OTTERY_INTERNAL
#include "ottery-internal.h"
#include "ottery.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#ifndef OTTERY_NO_LOCKS
#ifdef __APPLE__
#define OTTERY_OSATOMIC
#include <libkern/OSAtomic.h>
#else
#define OTTERY_PTHREADS
#include <pthread.h>
#endif
#endif

int
ottery_os_randbytes_(uint8_t *out, size_t outlen)
{
  int fd;
  ssize_t n;
  fd = open("/dev/urandom", O_RDONLY|O_CLOEXEC);
  if (fd < 0)
    return -1;
  if ((n = read(fd, out, outlen)) < 0 || (size_t)n != outlen)
    return -1;
  close(fd);
  return 0;
}

struct ottery_config {
  const struct ottery_prf *impl;
};

struct ottery_state {/*XXXX test this with sentinels and magic stuff */
  uint8_t buffer[MAX_OUTPUT_LEN];/*XXXX appears to need to be vector-aligned*/
  struct ottery_prf prf;
  uint32_t block_counter;
  uint8_t pos;
  uint8_t initialized;
  pid_t pid;
#if defined(OTTERY_OSATOMIC)
  OSSpinLock mutex;
#elif defined(OTTERY_PTHREADS)
  pthread_mutex_t mutex;
#endif
  uint8_t state[MAX_STATE_LEN];
};

static void
ottery_memclear_(void *mem, size_t len)
{
  /* XXXX Test on many many platforms. */
  volatile uint8_t *cp = mem;
  while (len--)
    *cp++ = 0;
}

#if defined(OTTERY_PTHREADS)
#define LOCK(st) do {                             \
    pthread_mutex_lock(&(st)->mutex);           \
  } while (0)
#define UNLOCK(st) do {                             \
    pthread_mutex_unlock(&(st)->mutex);             \
  } while (0)
#elif defined(OTTERY_OSATOMIC)
#define LOCK(st) do {                           \
    OSSpinLockLock(&(st)->mutex);               \
  } while(0)
#define UNLOCK(st) do {                         \
    OSSpinLockUnlock(&(st)->mutex);            \
  } while(0)
#elif defined(OTTERY_NO_LOCKS)
#define LOCK(st) ((void)0)
#define UNLOCK(st) ((void)0)
#else
#error How do I lock?
#endif

int
ottery_config_init(struct ottery_config *cfg)
{
  cfg->impl = &ottery_prf_chacha20_;
  return 0;
}

int
ottery_config_force_implementation(struct ottery_config *cfg,
                                   const char *impl)
{
  int i;
  static const struct {
    const char *name;
    const struct ottery_prf *prf;
  } prf_table[] = {
    { OTTERY_CHACHA,   &ottery_prf_chacha20_, },
    { OTTERY_CHACHA8,  &ottery_prf_chacha8_, },
    { OTTERY_CHACHA12, &ottery_prf_chacha12_, },
    { OTTERY_CHACHA20, &ottery_prf_chacha20_, },
    { NULL, NULL }
  };
  if (!impl)
    return -1;
  for (i = 0; prf_table[i].name; ++i) {
    if (0 == strcmp(impl, prf_table[i].name)) {
      cfg->impl = prf_table[i].prf;
      return 0;
    }
  }
  return -1;
}

static void ottery_st_stir_nolock(struct ottery_state *st);

static void
ottery_st_nextblock_nolock_nostir(struct ottery_state *st, uint8_t *target)
{
  st->prf.generate(st->state, target, st->block_counter);
  st->block_counter += st->prf.idx_step;
}

static void
ottery_st_nextblock_nolock(struct ottery_state *st, uint8_t *target)
{
  ottery_st_nextblock_nolock_nostir(st, target);
  if (st->block_counter >= 256)
    ottery_st_stir_nolock(st);
}

static int
ottery_st_initialize(struct ottery_state *st,
                     const struct ottery_prf *prf,
                     int reinit)
{
  if (!reinit) {
    memset(st, 0, sizeof(*st));
#ifdef OTTERY_PTHREADS
    if (!pthread_mutex_init(&st->mutex, NULL))
      return -1;
#endif
    if (prf->state_len > MAX_STATE_LEN)
      return -1;
    memcpy(&st->prf, prf, sizeof(*prf));
  }
  if (ottery_os_randbytes_(st->buffer, prf->state_bytes) < 0)
    return -1;
  prf->setup(st->state, st->buffer);
  st->block_counter = 0;

  ottery_st_nextblock_nolock(st, st->buffer);
  st->pos=0;

  st->pid = getpid();
  st->initialized = 1;
  return 0;
}

int
ottery_st_init(struct ottery_state *st, const struct ottery_config *cfg)
{
  return ottery_st_initialize(st, cfg->impl, 0);
}

void
ottery_st_add_seed(struct ottery_state *st, const uint8_t *seed, size_t n)
{
  uint8_t tmp_seed[MAX_STATE_BYTES];
  if (! seed) {
    unsigned state_bytes;
    LOCK(st);
    state_bytes = st->prf.state_bytes;
    UNLOCK(st);
    ottery_os_randbytes_(tmp_seed, state_bytes);
    seed = tmp_seed;
    n = state_bytes;
  }

  LOCK(st);
  while (n) {
    unsigned i;
    size_t m = n > st->prf.state_bytes ? st->prf.state_bytes : n;
    ottery_st_nextblock_nolock(st, st->buffer);
    for (i = 0; i < m; ++i) {
      st->buffer[i] ^= seed[i];
    }
    st->prf.setup(st->state, st->buffer);
    st->block_counter = 0;
    n -= m;
    seed += m;
  }
  ottery_st_nextblock_nolock(st, st->buffer);

  st->pos = 0;
  UNLOCK(st);

  if (seed == tmp_seed)
    ottery_memclear_(tmp_seed, sizeof(tmp_seed));
}

void
ottery_st_wipe(struct ottery_state *st)
{
#ifdef OTTERY_PTHREADS
  pthread_mutex_destroy(&st->mutex);
#endif
  ottery_memclear_(st, sizeof(struct ottery_state));
}

static void
ottery_st_stir_nolock(struct ottery_state *st)
{
  if (st->pos + st->prf.state_bytes > st->prf.output_len) {
    ottery_st_nextblock_nolock_nostir(st, st->buffer);
    st->pos = 0;
  }
  st->prf.setup(st->state, st->buffer + st->pos);
  st->block_counter = 0;
  ottery_st_nextblock_nolock_nostir(st, st->buffer);
  st->pos=0;
}

void
ottery_st_stir(struct ottery_state *st)
{
  LOCK(st);
  ottery_st_stir_nolock(st);
  UNLOCK(st);
}

void
ottery_st_rand_bytes(struct ottery_state *st, void *out_,
                     size_t n)
{
#ifndef OTTERY_NO_INIT_CHECK
  if (!st->initialized)
    abort();
#endif

  LOCK(st);
#ifndef OTTERY_NO_PID_CHECK
  if (st->pid != getpid()) {
    if (ottery_st_initialize(st, NULL, 1) < 0)
      abort();
  }
#endif

  uint8_t *out = out_;
  while (n >= st->prf.output_len) {
    ottery_st_nextblock_nolock(st, out);
    out += st->prf.output_len;
    n -= st->prf.output_len;
  }

  if (n + st->pos < st->prf.output_len) {
    memcpy(out, st->buffer+st->pos, n);
    st->pos += n;
  } else {
    unsigned cpy = st->prf.output_len - st->pos;
    memcpy(out, st->buffer+st->pos, cpy);
    n -= cpy;
    out += cpy;
    ottery_st_nextblock_nolock(st, st->buffer);
    memcpy(out, st->buffer, n);
    st->pos = n;
  }

  UNLOCK(st);
}

static inline void
ottery_st_rand_bytes_small(struct ottery_state *st, void *out_,
                           size_t n)
{
#ifndef OTTERY_NO_INIT_CHECK
  if (!st->initialized)
    abort();
#endif

  LOCK(st);
#ifndef OTTERY_NO_PID_CHECK
  if (st->pid != getpid()) {
    if (ottery_st_initialize(st, NULL, 1) < 0)
      abort();
  }
#endif

  uint8_t *out = out_;
  if (n + st->pos < st->prf.output_len) {
    memcpy(out, st->buffer+st->pos, n);
    st->pos += n;
  } else if (n + st->pos == st->prf.output_len) {
    memcpy(out, st->buffer+st->pos, n);
    ottery_st_nextblock_nolock(st, st->buffer);
    st->pos = 0;
  } else {
    unsigned cpy = st->prf.output_len - st->pos;
    memcpy(out, st->buffer+st->pos, cpy);
    n -= cpy;
    out += cpy;
    ottery_st_nextblock_nolock(st, st->buffer);
    memcpy(out, st->buffer, n);
    st->pos = n;
  }

  UNLOCK(st);
}

unsigned
ottery_st_rand_unsigned(struct ottery_state *st)
{
  unsigned u;
  ottery_st_rand_bytes_small(st, &u, sizeof(u));
  return u;
}

uint64_t
ottery_st_rand_uint64(struct ottery_state *st)
{
  uint64_t u;
  ottery_st_rand_bytes_small(st, &u, sizeof(u));
  return u;
}

unsigned
ottery_st_rand_range(struct ottery_state *st, unsigned upper)
{
  unsigned divisor = UINT_MAX / upper;
  unsigned n;
  do {
    n = (ottery_st_rand_unsigned(st) / divisor);
  } while (n > upper);

  return n;
}

uint64_t
ottery_st_rand_range64(struct ottery_state *st, uint64_t upper)
{
  unsigned divisor = UINT64_MAX / upper;
  unsigned n;
  do {
    n = (ottery_st_rand_uint64(st) / divisor);
  } while (n > upper);

  return n;
}

static int ottery_global_state_initialized_ = 0;
static struct ottery_state ottery_global_state_;

#define CHECK_INIT() do {                       \
    if (!ottery_global_state_initialized_) {    \
      if (ottery_init(NULL))                    \
        abort();                                \
    }                                           \
  } while (0)

int
ottery_init(const struct ottery_config *cfg)
{
  int n = ottery_st_init(&ottery_global_state_, cfg);
  if (n == 0)
    ottery_global_state_initialized_ = 1;
  return n;
}

void
ottery_add_seed(const uint8_t *seed, size_t n)
{
  CHECK_INIT();
  ottery_st_add_seed(&ottery_global_state_, seed, n);
}

void
ottery_wipe(void)
{
  if (ottery_global_state_initialized_) {
    ottery_global_state_initialized_ = 0;
    ottery_st_wipe(&ottery_global_state_);
  }
}

void
ottery_stir(void)
{
  CHECK_INIT();
  ottery_st_stir(&ottery_global_state_);
}

void
ottery_rand_bytes(void *out, size_t n)
{
  CHECK_INIT();
  ottery_st_rand_bytes(&ottery_global_state_, out, n);
}

unsigned
ottery_rand_unsigned(void)
{
  CHECK_INIT();
  return ottery_st_rand_unsigned(&ottery_global_state_);
}
uint64_t
ottery_rand_uint64(void)
{
  CHECK_INIT();
  return ottery_st_rand_uint64(&ottery_global_state_);
}
unsigned
ottery_rand_range(unsigned top)
{
  CHECK_INIT();
  return ottery_st_rand_range(&ottery_global_state_, top);
}
uint64_t
ottery_rand_range64(uint64_t top)
{
  CHECK_INIT();
  return ottery_st_rand_range64(&ottery_global_state_, top);
}
