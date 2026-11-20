// Ed25519 verification (RFC 8032) on TweetNaCl's arithmetic.
//
// SOURCE. Everything between the two "TweetNaCl" rules below is TweetNaCl,
// version 20140427, by Daniel J. Bernstein, Bernard van Gastel, Wesley
// Janssen, Tanja Lange, Peter Schwabe and Sjaak Smetsers: "a self-contained
// public-domain C library" (https://tweetnacl.cr.yp.to/).
//
//     https://tweetnacl.cr.yp.to/20140427/tweetnacl.c
//     sha256 02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4
//
// The functions are copied as published, in TweetNaCl's own layout so they
// can be compared with the original line for line: the field arithmetic mod
// 2^255 - 19 (set25519 car25519 sel25519 pack25519 neq25519 par25519
// unpack25519 A Z M S inv25519 pow2523), the group operations (add cswap pack
// scalarmult scalarbase unpackneg), reduction mod L (modL reduce), vn and
// crypto_verify_32, and the constants they use. Changed, and nothing else:
//
//   - everything is static, and the typedefs come from <stdint.h> (TweetNaCl's
//     u32 is unsigned long, which is 64 bits on a host);
//   - gf0 has an explicit = {0}, which C++ requires of a const, and two
//     trailing blanks are gone;
//   - two left shifts of a value that can be negative are multiplications:
//     "c<<16" in car25519 and "carry << 8" in modL. Shifting a negative number
//     left is undefined in C and in C++17, and UBSan stops the host tests on
//     it; the multiplication is the same arithmetic.
//
// Not taken: TweetNaCl's SHA-512 (mbedTLS's is in the firmware already),
// signing, and crypto_sign_open. ed25519_verify, after the second rule, keeps
// crypto_sign_open's order of operations and adds what RFC 8032 section 5.1.7
// requires and crypto_sign_open does not do:
//
//   - S below L. Without it S + L verifies wherever S does (malleability).
//   - A and R decoded strictly (section 5.1.3). unpack25519 drops bit 255 and
//     reads any y below 2^255, so y >= p would be taken mod p, and unpackneg
//     accepts x = 0 with the sign bit set. crypto_sign_open never decodes R at
//     all; it compares encodings, which refuses an R that does not decode
//     anyway, and the explicit decode is what the RFC says to do.
//   - The hash is streamed over R || A || M rather than over a copy.
//
// The check is cofactorless, as crypto_sign_open's is: [S]B - [k]A is
// encoded and compared with R, which with R decoded is [S]B = R + [k]A.
//
// COST. TweetNaCl is the smallest of the public-domain implementations and the
// slowest: a field element is sixteen 64-bit limbs, and both scalar
// multiplications are the plain 256-step ladder, so one verify is about 10,800
// field multiplications. Built as the firmware builds it (-Os, Cortex-M33) and
// counted on Renode's model of that core, one verify of a 64-byte message is
// 65 million instructions; an mbedTLS ECDSA P-256 verify, counted the same
// way, is 19 million. At 1.0 to 1.5 cycles an instruction that is 0.43 to
// 0.65 s at 150 MHz. -O2 and -O3 execute more instructions, not fewer, and -O3
// makes this file 14 KB instead of 4. DEVICE-UNCONFIRMED: counted on a model
// of the core, not timed on an RP2350; the cycles per instruction are an
// assumption.
//
// STACK. A gf is 128 bytes. The deepest point is equation()'s two points
// (1.1 KB with the rest of its frame), scalarbase's point (0.5 KB), add's nine
// temporaries (1.2 KB) and M (0.3 KB): 3.2 KB below ed25519_verify at -Os, by
// -fstack-usage. From the sshd task's entry that is 5.4 KB of its 8 KB, the
// deepest path the server has.
#include "ed25519.h"

#include "mbedtls/sha512.h"

// Three of TweetNaCl's functions kept out of line, by declaration only (their
// definitions below are untouched). Each is called once, so the compiler
// folds it into its caller and its points then sit in the caller's frame for
// the whole verify, under the 1.2 KB that add() needs: 300 bytes more at the
// deepest point.
static void scalarbase(int64_t p[4][16], const uint8_t *s) __attribute__((noinline));
static void pack(uint8_t *r, int64_t p[4][16]) __attribute__((noinline));
static void reduce(uint8_t *r) __attribute__((noinline));

// ---- TweetNaCl 20140427 -----------------------------------------------------------

// vn compares an unsigned index with an int bound, as published.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"

#define FOR(i,n) for (i = 0;i < n;++i)
#define sv static void

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;
typedef i64 gf[16];

static const gf
  gf0 = {0},
  gf1 = {1},
  D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203},
  D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406},
  X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169},
  Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666},
  I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

static int vn(const u8 *x,const u8 *y,int n)
{
  u32 i,d = 0;
  FOR(i,n) d |= x[i]^y[i];
  return (1 & ((d - 1) >> 8)) - 1;
}

static int crypto_verify_32(const u8 *x,const u8 *y)
{
  return vn(x,y,32);
}

sv set25519(gf r, const gf a)
{
  int i;
  FOR(i,16) r[i]=a[i];
}

sv car25519(gf o)
{
  int i;
  i64 c;
  FOR(i,16) {
    o[i]+=(1LL<<16);
    c=o[i]>>16;
    o[(i+1)*(i<15)]+=c-1+37*(c-1)*(i==15);
    o[i]-=c*65536;
  }
}

sv sel25519(gf p,gf q,int b)
{
  i64 t,i,c=~(b-1);
  FOR(i,16) {
    t= c&(p[i]^q[i]);
    p[i]^=t;
    q[i]^=t;
  }
}

sv pack25519(u8 *o,const gf n)
{
  int i,j,b;
  gf m,t;
  FOR(i,16) t[i]=n[i];
  car25519(t);
  car25519(t);
  car25519(t);
  FOR(j,2) {
    m[0]=t[0]-0xffed;
    for(i=1;i<15;i++) {
      m[i]=t[i]-0xffff-((m[i-1]>>16)&1);
      m[i-1]&=0xffff;
    }
    m[15]=t[15]-0x7fff-((m[14]>>16)&1);
    b=(m[15]>>16)&1;
    m[14]&=0xffff;
    sel25519(t,m,1-b);
  }
  FOR(i,16) {
    o[2*i]=t[i]&0xff;
    o[2*i+1]=t[i]>>8;
  }
}

static int neq25519(const gf a, const gf b)
{
  u8 c[32],d[32];
  pack25519(c,a);
  pack25519(d,b);
  return crypto_verify_32(c,d);
}

static u8 par25519(const gf a)
{
  u8 d[32];
  pack25519(d,a);
  return d[0]&1;
}

sv unpack25519(gf o, const u8 *n)
{
  int i;
  FOR(i,16) o[i]=n[2*i]+((i64)n[2*i+1]<<8);
  o[15]&=0x7fff;
}

sv A(gf o,const gf a,const gf b)
{
  int i;
  FOR(i,16) o[i]=a[i]+b[i];
}

sv Z(gf o,const gf a,const gf b)
{
  int i;
  FOR(i,16) o[i]=a[i]-b[i];
}

sv M(gf o,const gf a,const gf b)
{
  i64 i,j,t[31];
  FOR(i,31) t[i]=0;
  FOR(i,16) FOR(j,16) t[i+j]+=a[i]*b[j];
  FOR(i,15) t[i]+=38*t[i+16];
  FOR(i,16) o[i]=t[i];
  car25519(o);
  car25519(o);
}

sv S(gf o,const gf a)
{
  M(o,a,a);
}

sv inv25519(gf o,const gf i)
{
  gf c;
  int a;
  FOR(a,16) c[a]=i[a];
  for(a=253;a>=0;a--) {
    S(c,c);
    if(a!=2&&a!=4) M(c,c,i);
  }
  FOR(a,16) o[a]=c[a];
}

sv pow2523(gf o,const gf i)
{
  gf c;
  int a;
  FOR(a,16) c[a]=i[a];
  for(a=250;a>=0;a--) {
    S(c,c);
    if(a!=1) M(c,c,i);
  }
  FOR(a,16) o[a]=c[a];
}

sv add(gf p[4],gf q[4])
{
  gf a,b,c,d,t,e,f,g,h;

  Z(a, p[1], p[0]);
  Z(t, q[1], q[0]);
  M(a, a, t);
  A(b, p[0], p[1]);
  A(t, q[0], q[1]);
  M(b, b, t);
  M(c, p[3], q[3]);
  M(c, c, D2);
  M(d, p[2], q[2]);
  A(d, d, d);
  Z(e, b, a);
  Z(f, d, c);
  A(g, d, c);
  A(h, b, a);

  M(p[0], e, f);
  M(p[1], h, g);
  M(p[2], g, f);
  M(p[3], e, h);
}

sv cswap(gf p[4],gf q[4],u8 b)
{
  int i;
  FOR(i,4)
    sel25519(p[i],q[i],b);
}

sv pack(u8 *r,gf p[4])
{
  gf tx, ty, zi;
  inv25519(zi, p[2]);
  M(tx, p[0], zi);
  M(ty, p[1], zi);
  pack25519(r, ty);
  r[31] ^= par25519(tx) << 7;
}

sv scalarmult(gf p[4],gf q[4],const u8 *s)
{
  int i;
  set25519(p[0],gf0);
  set25519(p[1],gf1);
  set25519(p[2],gf1);
  set25519(p[3],gf0);
  for (i = 255;i >= 0;--i) {
    u8 b = (s[i/8]>>(i&7))&1;
    cswap(p,q,b);
    add(q,p);
    add(p,p);
    cswap(p,q,b);
  }
}

sv scalarbase(gf p[4],const u8 *s)
{
  gf q[4];
  set25519(q[0],X);
  set25519(q[1],Y);
  set25519(q[2],gf1);
  M(q[3],X,Y);
  scalarmult(p,q,s);
}

static const u64 L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

sv modL(u8 *r,i64 x[64])
{
  i64 carry,i,j;
  for (i = 63;i >= 32;--i) {
    carry = 0;
    for (j = i - 32;j < i - 12;++j) {
      x[j] += carry - 16 * x[i] * L[j - (i - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j] += carry;
    x[i] = 0;
  }
  carry = 0;
  FOR(j,32) {
    x[j] += carry - (x[31] >> 4) * L[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  FOR(j,32) x[j] -= carry * L[j];
  FOR(i,32) {
    x[i+1] += x[i] >> 8;
    r[i] = x[i] & 255;
  }
}

sv reduce(u8 *r)
{
  i64 x[64],i;
  FOR(i,64) x[i] = (u64) r[i];
  FOR(i,64) r[i] = 0;
  modL(r,x);
}

static int unpackneg(gf r[4],const u8 p[32])
{
  gf t, chk, num, den, den2, den4, den6;
  set25519(r[2],gf1);
  unpack25519(r[1],p);
  S(num,r[1]);
  M(den,num,D);
  Z(num,num,r[2]);
  A(den,r[2],den);

  S(den2,den);
  S(den4,den2);
  M(den6,den4,den2);
  M(t,den6,num);
  M(t,t,den);

  pow2523(t,t);
  M(t,t,num);
  M(t,t,den);
  M(t,t,den);
  M(r[0],t,den);

  S(chk,r[0]);
  M(chk,chk,den);
  if (neq25519(chk, num)) M(r[0],r[0],I);

  S(chk,r[0]);
  M(chk,chk,den);
  if (neq25519(chk, num)) return -1;

  if (par25519(r[0]) == (p[31]>>7)) Z(r[0],gf0,r[0]);

  M(r[3],r[0],r[1]);
  return 0;
}

#undef FOR
#undef sv
#pragma GCC diagnostic pop

// ---- end of TweetNaCl ---------------------------------------------------------------

// S < L, with S and L both little-endian, compared from the top byte down. S
// is public, so this need not be constant-time.
static bool scalar_in_range(const u8 s[32]) {
    for (int i = 31; i >= 0; i--) {
        if (s[i] < L[i]) return true;
        if (s[i] > L[i]) return false;
    }
    return false;               // S == L
}

// RFC 8032 section 5.1.3, into the NEGATED point, which is what the verify
// needs of A. y >= p fails: unpack25519 reads y mod p, so a y that does not
// pack back to the same bytes was not below p. A y with no x fails in
// unpackneg. x = 0 with the sign bit set fails here; unpackneg's negation of
// zero is still zero, so it can be looked at afterwards.
//
// The functions from here down are noinline on purpose, so that each frame
// holds only its own points: inlined into one another, the verify's frame was
// 2 KB and its deepest path 1 KB longer.
#define NOINLINE __attribute__((noinline))

static NOINLINE bool decode_neg(gf r[4], const u8 enc[32]) {
    gf y;
    u8 c[32];
    unpack25519(y, enc);
    pack25519(c, y);
    c[31] |= enc[31] & 0x80;
    if (vn(c, enc, 32) != 0) return false;
    if (unpackneg(r, enc) != 0) return false;
    if ((enc[31] >> 7) && !neq25519(r[0], gf0)) return false;
    return true;
}

NOINLINE bool ed25519_point_ok(const uint8_t enc[32]) {
    gf r[4];
    return decode_neg(r, enc);
}

// SHA-512(R || A || M), section 5.1.7 step 2, before reduction mod L.
static NOINLINE bool hash_k(u8 h[64], const u8 *r, const u8 *a, const u8 *m, size_t n) {
    mbedtls_sha512_context c;
    mbedtls_sha512_init(&c);
    bool ok = mbedtls_sha512_starts(&c, 0) == 0 &&
              mbedtls_sha512_update(&c, r, 32) == 0 &&
              mbedtls_sha512_update(&c, a, 32) == 0 &&
              (n == 0 || mbedtls_sha512_update(&c, m, n) == 0) &&
              mbedtls_sha512_finish(&c, h) == 0;
    mbedtls_sha512_free(&c);
    return ok;
}

// A decoded, k, and then [S]B - [k]A as crypto_sign_open computes it,
// compared with R. Cofactorless (step 3): [S]B = R + [k]A. R was decoded
// already and both encodings are canonical, so equal bytes are an equal point.
static NOINLINE bool equation(const u8 sig[64], const u8 *msg, size_t n, const u8 pk[32]) {
    gf p[4], q[4];
    u8 h[64], t[32];
    if (!decode_neg(q, pk)) return false;          // q = -A
    if (!hash_k(h, sig, pk, msg, n)) return false;
    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    add(p, q);
    pack(t, p);
    return crypto_verify_32(sig, t) == 0;
}

bool ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32]) {
    // Step 1, the cheapest refusals first: S below L, then R decodes. A is
    // decoded in equation(), where its point is kept.
    if (!scalar_in_range(sig + 32)) return false;
    if (!ed25519_point_ok(sig)) return false;
    return equation(sig, msg, n, pk);
}
