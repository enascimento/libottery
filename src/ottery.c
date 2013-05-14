#define OTTERY_INTERNAL
#include "ottery-internal.h"
#include "ottery.h"
#include "ottery_st.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#ifndef OTTERY_NO_LOCKS
#if defined(__APPLE__) && !defined(OTTERY_NO_SPINLOCKS)
#define OTTERY_OSATOMIC
#include <libkern/OSAtomic.h>
#elif defined(_WIN32)
#define OTTERY_CRITICAL_SECTION
#include <windows.h>
#else
#define OTTERY_PTHREADS
#include <pthread.h>
#endif
#endif /* OTTERY_NO_LOCKS */

#ifdef _WIN32
/* On Windows, there is no fork(), so we don't need to worry about forking. */
#define OTTERY_NO_PID_CHECK
#endif

/**
 * Evaluate the condition 'x', while hinting to the compiler that it is
 * likely to be false.
 */
#define UNLIKELY(x) __builtin_expect((x), 0)

int
ottery_os_randbytes_(uint8_t *out, size_t outlen)
{
#ifdef _WIN32
  /* On Windows, CryptGenRandom is supposed to be a well-seeded
   * cryptographically strong random number generator. */
  HCRYPTPROV provider;
  int retval = 0;
  if (0 == CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL,
                               CRYPT_VERIFYCONTEXT))
    return OTTERY_ERR_INIT_STRONG_RNG;

  if (0 == CryptGenRandom(provider, outlen, out))
    retval = OTTERY_ERR_ACCESS_STRONG_RNG;

  CryptReleaseContext(provider, 0);
  return retval;
#else
  /* On most unixes these days, you can get strong random numbers from
   * /dev/urandom.
   *
   * That's assuming that /dev/urandom is seeded.  For most applications,
   * that won't be a problem. But for stuff that starts close to system
   * startup, before the operating system has added any entropy to the pool,
   * it can be pretty bad.
   *
   * You could use /dev/random instead, if you want, but that has another
   * problem.  It will block if the OS PRNG has received less entropy than
   * it has emitted.  If we assume that the OS PRNG isn't cryptographically
   * weak, blocking in that case is simple overkill.
   *
   * It would be best if there were an alternative that blocked if the the
   * PRNG had _never_ been seeded.  But most operating systems don't have
   * that.
   */
  int fd;
  ssize_t n;
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
  fd = open("/dev/urandom", O_RDONLY|O_CLOEXEC);
  if (fd < 0)
    return OTTERY_ERR_INIT_STRONG_RNG;
  if ((n = read(fd, out, outlen)) < 0 || (size_t)n != outlen)
    return OTTERY_ERR_ACCESS_STRONG_RNG;
  close(fd);
  return 0;
#endif
}

/** Magic number for deciding whether an ottery_state is initialized. */
#define MAGIC_BASIS 0x11b07734

/** Macro: yield the correct magic number for an ottery_state, based on
 * its position in RAM. */
#define MAGIC(ptr) ( ((uint32_t)(uintptr_t)(ptr)) ^ MAGIC_BASIS )

struct ottery_config {
  /** The PRF that we should use.  If NULL, we use the default. */
  const struct ottery_prf *impl;
};

struct __attribute__((aligned(16))) ottery_state {
  /**
   * Holds up to prf.output_len bytes that have been generated by the
   * pseudorandom function. */
  __attribute__ ((aligned (16))) uint8_t buffer[MAX_OUTPUT_LEN];
  /**
   * Holds the state information (typically nonces and keys) used by the
   * pseudorandom function. */

  __attribute__ ((aligned (16))) uint8_t state[MAX_STATE_LEN];
  /**
   * Parameters and function pointers for the cryptographic pseudorandom
   * function that we're using. */
  struct ottery_prf prf;
  /**
   * Index of the *next* block counter to use when generating random bytes
   * with prf.  When this equals or exceeds prf.stir_after, we should stir
   * the PRNG. */
  uint32_t block_counter;
  /**
   * Magic number; used to tell whether this state is initialized.
   */
  uint32_t magic;
  /**
   * Index of the next byte in (buffer) to yield to the user.
   *
   * Invariant: this is less than prf.output_len. */
  uint8_t pos;
  /**
   * The pid of the process in which this PRF was most recently seeded
   * from the OS. We use this to avoid use-after-fork problems; see
   * ottery_st_rand_lock_and_check(). */
  pid_t pid;
  /**
   * @brief Locks for this structure.
   *
   * This lock will not necessarily be recursive.  It's probably a
   * spinlock.
   *
   * @{
   */
#if defined(OTTERY_OSATOMIC)
  OSSpinLock mutex;
#elif defined(OTTERY_CRITICAL_SECTION)
  CRITICAL_SECTION mutex;
#elif defined(OTTERY_PTHREADS)
  pthread_mutex_t mutex;
#endif
  /**@}*/
};

size_t
ottery_get_sizeof_config(void)
{
  return sizeof(struct ottery_config);
}

size_t
ottery_get_sizeof_state(void)
{
  return sizeof(struct ottery_state);
}

/**
 * Clear all bytes stored in a structure. Unlike memset, the compiler is not
 * going to optimize this out of existence because the target is about to go
 * out of scope.
 *
 * @param mem Pointer to the memory to erase.
 * @param len The number of bytes to erase.
 */
/* NOTE: whenever we change this, change test/test_memclear.c accordingly */
static void
ottery_memclear_(void *mem, size_t len)
{
  volatile uint8_t *cp = mem;
  while (len--)
    *cp++ = 0;
}


#if defined(OTTERY_PTHREADS)
/** Acquire the lock for the state "st". */
#define LOCK(st) do {                           \
    pthread_mutex_lock(&(st)->mutex);           \
  } while (0)
/** Release the lock for the state "st". */
#define UNLOCK(st) do {                             \
    pthread_mutex_unlock(&(st)->mutex);             \
  } while (0)
#elif defined(OTTERY_CRITICAL_SECTION)
#define LOCK(st) do { \
    EnterCriticalSection(&(st)->mutex); \
  } while (0)
#define UNLOCK(st) do { \
    LeaveCriticalSection(&(st)->mutex); \
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

/** Which PRF should we use by default? */
#define OTTERY_PRF_DEFAULT ottery_prf_chacha20_

int
ottery_config_init(struct ottery_config *cfg)
{
  cfg->impl = &OTTERY_PRF_DEFAULT;
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
    { OTTERY_PRF_CHACHA,   &ottery_prf_chacha20_, },
    { OTTERY_PRF_CHACHA8,  &ottery_prf_chacha8_, },
    { OTTERY_PRF_CHACHA12, &ottery_prf_chacha12_, },
    { OTTERY_PRF_CHACHA20, &ottery_prf_chacha20_, },

#ifdef OTTERY_HAVE_SIMD_IMPL
    { OTTERY_PRF_CHACHA_SIMD,   &ottery_prf_chacha20_krovetz_, },
    { OTTERY_PRF_CHACHA8_SIMD,  &ottery_prf_chacha8_krovetz_, },
    { OTTERY_PRF_CHACHA12_SIMD, &ottery_prf_chacha12_krovetz_, },
    { OTTERY_PRF_CHACHA20_SIMD, &ottery_prf_chacha20_krovetz_, },
#endif

    { OTTERY_PRF_CHACHA_NO_SIMD,   &ottery_prf_chacha20_merged_, },
    { OTTERY_PRF_CHACHA8_NO_SIMD,  &ottery_prf_chacha8_merged_, },
    { OTTERY_PRF_CHACHA12_NO_SIMD, &ottery_prf_chacha12_merged_, },
    { OTTERY_PRF_CHACHA20_NO_SIMD, &ottery_prf_chacha20_merged_, },

    { NULL, NULL }
  };
  if (!impl)
    return OTTERY_ERR_INVALID_ARGUMENT;
  for (i = 0; prf_table[i].name; ++i) {
    if (0 == strcmp(impl, prf_table[i].name)) {
      cfg->impl = prf_table[i].prf;
      return 0;
    }
  }
  return OTTERY_ERR_INVALID_ARGUMENT;
}

void
ottery_config_set_manual_prf_(struct ottery_config *cfg,
                              const struct ottery_prf *prf)
{
  cfg->impl = prf;
}

static void ottery_st_stir_nolock(struct ottery_state *st);

/**
 * As ottery_st_nextblock_nolock(), but never stir the state.
 */
static void
ottery_st_nextblock_nolock_nostir(struct ottery_state *st, uint8_t *target)
{
  st->prf.generate(st->state, target, st->block_counter);
  ++st->block_counter;
}

/**
 * Generate (st->output_len) bytes of pseudorandom data from the PRF.
 * Advance the block counter as appropriate. If the block counter becomes
 * too high, stir the state.
 *
 * This function does not acquire the lock on the state; use it within
 * another function that does.
 *
 * @param st The state to use when generating the block.
 * @param target The location to write to. Must be aligned to a 16-byte
 *    boundary.
 */
static void
ottery_st_nextblock_nolock(struct ottery_state *st, uint8_t *target)
{
  ottery_st_nextblock_nolock_nostir(st, target);
  if (UNLIKELY(st->block_counter >= st->prf.stir_after))
    ottery_st_stir_nolock(st);
}

/**
 * Initialize or reinitialize a PRNG state.
 *
 * @param st The state to initialize or reinitialize.
 * @param prf The PRF to use. (Ignored for reinit)
 * @return An OTTERY_ERR_* value (zero on success, nonzero on failure).
 */
static int
ottery_st_initialize(struct ottery_state *st,
                     const struct ottery_prf *prf,
                     int reinit)
{
  if (!reinit) {
    /* We really need our state to be aligned. If it isn't, let's give an
     * error now, and not a crash when the SIMD instructions start to fail.
     */
    if (((uintptr_t)st) & 0xf)
      return OTTERY_ERR_STATE_ALIGNMENT;

    memset(st, 0, sizeof(*st));

    /* Now set up the spinlock or mutex or hybrid thing. */
#ifdef OTTERY_PTHREADS
    if (pthread_mutex_init(&st->mutex, NULL))
      return OTTERY_ERR_LOCK_INIT;
#elif defined(OTTERY_CRITICAL_SECTION)
    if (InitializeCriticalSectionAndSpinCount(&st->mutex, 3000) == 0)
      return OTTERY_ERR_LOCK_INIT;
#elif defined(OTTERY_OSATOMIC)
    /* Setting an OSAtomic spinlock to 0 is all you need to do to
     * initialize it.*/
#endif

    /* Check invariants for PRF, in case we wrote some bad code. */
    if ((prf->state_len > MAX_STATE_LEN) ||
        (prf->state_bytes > MAX_STATE_BYTES) ||
        (prf->state_bytes > prf->output_len) ||
        (prf->output_len > MAX_OUTPUT_LEN))
      return OTTERY_ERR_INTERNAL;

    /* Check whether some of our structure size assumptions are right. */
    if ((sizeof(struct ottery_state) > 1024) ||
        (sizeof(struct ottery_config) > 1024))
      return OTTERY_ERR_INTERNAL;

    /* Copy the PRF into place. */
    memcpy(&st->prf, prf, sizeof(*prf));
  }
  /* Now seed the PRF: Generate some random bytes from the OS, and use them
   * as whatever keys/nonces/whatever the PRF wants to have. */
  int err;
  if ((err = ottery_os_randbytes_(st->buffer, prf->state_bytes)))
    return err;
  prf->setup(st->state, st->buffer);

  /* Generate the first block of output. */
  st->block_counter = 0;
  ottery_st_nextblock_nolock(st, st->buffer);
  st->pos=0;

  st->pid = getpid();

  /* Set the magic number last, or else we might look like we succeeded
   * when we didn't */
  st->magic = MAGIC(st);

  return 0;
}

int
ottery_st_init(struct ottery_state *st, const struct ottery_config *cfg)
{
  if (cfg) {
    return ottery_st_initialize(st, cfg->impl, 0);
  } else {
    return ottery_st_initialize(st, &OTTERY_PRF_DEFAULT, 0);
  }
}

void
ottery_st_add_seed(struct ottery_state *st, const uint8_t *seed, size_t n)
{
  /* If the user passed NULL, then we should reseed from the operating
   * system. */
  uint8_t tmp_seed[MAX_STATE_BYTES];

  /* XXXX check for initialization */

  if (! seed) {
    unsigned state_bytes;
    /* Hold the lock for only a moment here: we don't want to be holding
     * it while we call the OS RNG. */
    LOCK(st);
    state_bytes = st->prf.state_bytes;
    UNLOCK(st);
    ottery_os_randbytes_(tmp_seed, state_bytes);
    seed = tmp_seed;
    n = state_bytes;
  }

  if (n == 0)
    return;

  LOCK(st);
  /* The algorithm here is really easy. We grab a block of output from the
   * PRNG, that the first (state_bytes) bytes of that, XOR it with up to
   * (state_bytes) bytes of our new seed data, and use that to set our new
   * state. We do this over and over until we have no more seed data to add.
   */
  while (n) {
    unsigned i;
    size_t m = n > st->prf.state_bytes ? st->prf.state_bytes : n;
    ottery_st_nextblock_nolock_nostir(st, st->buffer);
    for (i = 0; i < m; ++i) {
      st->buffer[i] ^= seed[i];
    }
    st->prf.setup(st->state, st->buffer);
    st->block_counter = 0;
    n -= m;
    seed += m;
  }

  /* Now make sure that st->buffer is set up with the new state. */
  ottery_st_nextblock_nolock(st, st->buffer);
  st->pos = 0;

  UNLOCK(st);

  /* If we used stack-allocated seed material, wipe it. */
  if (seed == tmp_seed)
    ottery_memclear_(tmp_seed, sizeof(tmp_seed));
}

void
ottery_st_wipe(struct ottery_state *st)
{
#ifdef OTTERY_PTHREADS
  pthread_mutex_destroy(&st->mutex);
#elif defined(OTTERY_CRITICAL_SECTION)
  DeleteCriticalSection(&st->mutex);
#elif defined(OTTERY_OSATOMIC)
  /* You don't need to do anything to tear down an OSAtomic spinlock. */
#endif

  ottery_memclear_(st, sizeof(struct ottery_state));
}

/**
 * As ottery_st_stir, but do not acquire a lock on the state.
 *
 * @param st The state to stir.
 */
static void
ottery_st_stir_nolock(struct ottery_state *st)
{
  if (st->pos + st->prf.state_bytes > st->prf.output_len) {
    /* If we don't have enough data to generate the required (state_bytes)
     * worth of key material, we'll need to make one more block. */
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

/** Function that's invoked on a fatal error. See
 * ottery_set_fatal_handler() for more information. */
static void (*ottery_fatal_handler)(int) = NULL;

/** Called when a fatal error has occurred: Die horribly, or invoke
 * ottery_fatal_handler. */
static void
ottery_fatal(int error)
{
  if (ottery_fatal_handler)
    ottery_fatal_handler(error);
  else
    abort();
}

void
ottery_set_fatal_handler(void (*fn)(int))
{
  ottery_fatal_handler = fn;
}

static inline void ottery_st_rand_lock_and_check(struct ottery_state *st)
  __attribute__((always_inline));

/**
 * Shared prologue for functions generating random bytes from an
 * ottery_state.  Make sure that the state is initialized, lock it, and
 * reseed it (if the PID has changed).
 */
static inline void
ottery_st_rand_lock_and_check(struct ottery_state *st)
{
#ifndef OTTERY_NO_INIT_CHECK
  if (UNLIKELY(st->magic != MAGIC(st))) {
    ottery_fatal(OTTERY_ERR_STATE_INIT);
  }
#endif

  LOCK(st);
#ifndef OTTERY_NO_PID_CHECK
  if (UNLIKELY(st->pid != getpid())) {
    int err;
    if ((err = ottery_st_initialize(st, NULL, 1)))
      ottery_fatal(OTTERY_ERR_FLAG_POSTFORK_RESEED|err);
  }
#endif
}

/**
 * Generate a small-ish number of bytes from an ottery_state, using
 * buffered data.  If there is insufficient data in the buffer right now,
 * use what we have, and generate more.
 *
 * @param st The state to use.
 * @param out A location to write to.
 * @param n The number of bytes to write. Must not be greater than
 *     st->prf.output_len.
 */
static inline void
ottery_st_rand_bytes_from_buf(struct ottery_state *st, uint8_t *out,
                              size_t n)
{
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
}

void
ottery_st_rand_bytes(struct ottery_state *st, void *out_,
                     size_t n)
{
  ottery_st_rand_lock_and_check(st);

  uint8_t *out = out_;

  if (n >= st->prf.output_len) {
    /* We can generate at least a whole block, so let's try to write it
     * directly to the output, and not to the buffer. */

    if ( ((uintptr_t)out) & 0xf ) {
      /* The output pointer is misaligned, but prf.generate probably
       * requires an aligned pointer. So let's fill in the misaligned
       * portion at the front. */
      unsigned misalign = 16 - (((uintptr_t)out) & 0xf);
      ottery_st_rand_bytes_from_buf(st, out, misalign);
      out += misalign;
      n -= misalign;
    }

    /* Now we can do our bulk-write directly. Note that this can advance
     * the block_counter without changing the contents of st->buffer.
     * That's okay: once st->buffer runs out, it will get regenerated using
     * the new data. */
    while (n >= st->prf.output_len) {
      ottery_st_nextblock_nolock(st, out);
      out += st->prf.output_len;
      n -= st->prf.output_len;
    }
  }

  /* If there's more to write, but less than a full (output_len) bytes,
   * we should take it from our buffer. */
  if (n) {
    ottery_st_rand_bytes_from_buf(st, out, n);
  }

  UNLOCK(st);
}

#if defined (__amd64) || defined(__i386)
/**
 * Assign an integer type from bytes at a possibly unaligned pointer.
 *
 * @param type the type of integer to assign.
 * @param r the integer lvalue to write to.
 * @param p a pointer to the bytes to read from.
 **/
/* Unalignd integer access is allowed and fast */
#define INT_ASSIGN_PTR(type, r, p) do {         \
    r = *(type*)(p);                            \
  } while (0)
#else
#define INT_ASSIGN_PTR(type, r, p) do {         \
    memcpy(&r, p, sizeof(type));                \
  } while (0)
#endif

/**
 * Shared code for implementing rand_unsigned() and rand_uint64().
 *
 * @param st The state to use.
 * @param inttype The type of integer to generate.
 **/
#define OTTERY_RETURN_RAND_INTTYPE(st, inttype) do {                    \
    ottery_st_rand_lock_and_check(st);                                  \
    inttype result;                                                     \
    if (sizeof(inttype) + (st)->pos < (st)->prf.output_len) {           \
      INT_ASSIGN_PTR(inttype, result, (st)->buffer + (st)->pos);        \
      (st)->pos += sizeof(inttype);                                     \
    } else {                                                            \
      /* Our handling of this case here is significantly simpler */     \
      /* than that of ottery_st_rand_bytes_from_buf, at the expense */  \
      /* of wasting up to sizeof(inttype)-1 bytes. Since inttype */     \
      /* is at most 8 bytes long, that's not such a big deal. */        \
      ottery_st_nextblock_nolock(st, st->buffer);                       \
      INT_ASSIGN_PTR(inttype, result, (st)->buffer);                    \
      (st)->pos = sizeof(inttype);                                      \
    }                                                                   \
    UNLOCK(st);                                                         \
    return result;                                                      \
    } while (0)

unsigned
ottery_st_rand_unsigned(struct ottery_state *st)
{
  OTTERY_RETURN_RAND_INTTYPE(st, unsigned);
}

uint64_t
ottery_st_rand_uint64(struct ottery_state *st)
{
  OTTERY_RETURN_RAND_INTTYPE(st, uint64_t);
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

/** Flag: true iff ottery_global_state_ is initialized. */
static int ottery_global_state_initialized_ = 0;
/** A global state to use for the ottery_* functions that don't take a
 * state. */
static struct ottery_state ottery_global_state_;

/** Initialize ottery_global_state_ if it has not been initialize. */
#define CHECK_INIT() do {                                       \
      if (UNLIKELY(!ottery_global_state_initialized_))	 {      \
      int err;                                                  \
      if ((err=ottery_init(NULL)))                              \
        ottery_fatal(OTTERY_ERR_FLAG_GLOBAL_PRNG_INIT|err);     \
    }                                                           \
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
