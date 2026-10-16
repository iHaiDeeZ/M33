// Fast memcmp/bcmp/memcpy/memmove/memset for the PS4 eboot.
//
// The OpenOrbis libc.a we link has musl's portable C versions: memcmp compares one byte per loop
// iteration, memcpy has no SIMD at all. Dolphin and Mesa call them constantly (pipeline and
// shader cache lookups, texture hashing and uploads, state compares); the profiler showed
// memcmp alone at ~8% of the GPU thread in heavy scenes. These definitions take precedence over
// the archive members for the whole executable (Mesa included).
//
// SSE2 with unaligned loads (the PS4's Jaguar handles them well), aligned stores for long runs,
// and overlapping head/tail accesses instead of byte loops. `no_builtin` stops the compiler from
// turning the loops back into calls to these very functions.

#include <emmintrin.h>
#include <stddef.h>
#include <stdint.h>

#define PS4_STRING __attribute__((no_builtin, used))

typedef uint64_t __attribute__((aligned(1), may_alias)) u64_unaligned;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32_unaligned;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16_unaligned;

static inline __m128i load16(const unsigned char* p)
{
  return _mm_loadu_si128((const __m128i*)p);
}

static inline void store16(unsigned char* p, __m128i v)
{
  _mm_storeu_si128((__m128i*)p, v);
}

PS4_STRING int memcmp(const void* a, const void* b, size_t n)
{
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  while (n >= 16)
  {
    const unsigned differ =
        (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(load16(x), load16(y))) ^ 0xffffu;
    if (differ)
    {
      const unsigned i = (unsigned)__builtin_ctz(differ);
      return (int)x[i] - (int)y[i];
    }
    x += 16;
    y += 16;
    n -= 16;
  }
  if (n >= 8 && *(const u64_unaligned*)x == *(const u64_unaligned*)y)
  {
    x += 8;
    y += 8;
    n -= 8;
  }
  for (; n; n--, x++, y++)
  {
    if (*x != *y)
      return (int)*x - (int)*y;
  }
  return 0;
}

PS4_STRING int bcmp(const void* a, const void* b, size_t n)
{
  return memcmp(a, b, n);
}

// Forward copy of non-overlapping ranges.
static inline void copy_forward(unsigned char* d, const unsigned char* s, size_t n)
{
  if (n <= 16)
  {
    if (n >= 8)
    {
      const uint64_t lo = *(const u64_unaligned*)s, hi = *(const u64_unaligned*)(s + n - 8);
      *(u64_unaligned*)d = lo;
      *(u64_unaligned*)(d + n - 8) = hi;
    }
    else if (n >= 4)
    {
      const uint32_t lo = *(const u32_unaligned*)s, hi = *(const u32_unaligned*)(s + n - 4);
      *(u32_unaligned*)d = lo;
      *(u32_unaligned*)(d + n - 4) = hi;
    }
    else if (n >= 2)
    {
      const uint16_t lo = *(const u16_unaligned*)s, hi = *(const u16_unaligned*)(s + n - 2);
      *(u16_unaligned*)d = lo;
      *(u16_unaligned*)(d + n - 2) = hi;
    }
    else if (n)
    {
      *d = *s;
    }
    return;
  }
  if (n <= 32)
  {
    const __m128i lo = load16(s), hi = load16(s + n - 16);
    store16(d, lo);
    store16(d + n - 16, hi);
    return;
  }
  const __m128i head = load16(s);
  const __m128i tail0 = load16(s + n - 32), tail1 = load16(s + n - 16);
  // Aligned stores from the first 16-byte boundary after d (the head store covers the start).
  size_t i = 16 - ((uintptr_t)d & 15);
  store16(d, head);
  for (; i + 32 <= n; i += 32)
  {
    const __m128i v0 = load16(s + i), v1 = load16(s + i + 16);
    _mm_store_si128((__m128i*)(d + i), v0);
    _mm_store_si128((__m128i*)(d + i + 16), v1);
  }
  store16(d + n - 32, tail0);
  store16(d + n - 16, tail1);
}

PS4_STRING void* memcpy(void* __restrict dst, const void* __restrict src, size_t n)
{
  copy_forward((unsigned char*)dst, (const unsigned char*)src, n);
  return dst;
}

PS4_STRING void* memmove(void* dst, const void* src, size_t n)
{
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  if (d + n <= s || s + n <= d)
  {
    copy_forward(d, s, n);
    return dst;
  }
  if (d < s)
  {
    // Overlapping, dst below src: forward, each block loaded before its store (later blocks lie
    // above everything stored so far).
    size_t i = 0;
    for (; i + 16 <= n; i += 16)
      store16(d + i, load16(s + i));
    for (; i < n; i++)
      d[i] = s[i];
    return dst;
  }
  // Overlapping, dst above src: backwards, each block loaded before its store.
  size_t i = n;
  for (; i >= 16; i -= 16)
    store16(d + i - 16, load16(s + i - 16));
  while (i)
  {
    i--;
    d[i] = s[i];
  }
  return dst;
}

PS4_STRING void* memset(void* dst, int c, size_t n)
{
  unsigned char* p = (unsigned char*)dst;
  if (n < 16)
  {
    for (size_t i = 0; i < n; i++)
      p[i] = (unsigned char)c;
    return dst;
  }
  const __m128i v = _mm_set1_epi8((char)c);
  store16(p, v);
  store16(p + n - 16, v);
  for (size_t i = 16 - ((uintptr_t)p & 15); i + 16 <= n; i += 16)
    _mm_store_si128((__m128i*)(p + i), v);
  return dst;
}
