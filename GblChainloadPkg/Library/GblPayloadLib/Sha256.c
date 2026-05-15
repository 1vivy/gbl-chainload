/* GblChainloadPkg/Library/GblPayloadLib/Sha256.c
   Host: libcrypto. EDK2: vendored public-domain SHA-256. */
#include "Internal/Sha256.h"

#ifdef GBL_HOST_BUILD
# include <openssl/sha.h>
void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]) {
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, buf, len);
    SHA256_Final(out, &c);
}
#else
/* Vendored public-domain SHA-256 (Brad Conte / B-Con/crypto-algorithms,
   public domain; ~100 LOC). Avoids pulling BaseCryptLib + OpensslLib
   into the GblChainload binary just for one hash function. */

#include <Library/BaseMemoryLib.h>

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22))
#define EP1(x) (ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25))
#define SIG0(x) (ROTR(x,7) ^ ROTR(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

static const UINT32 k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

typedef struct {
  UINT8  data[64];
  UINT32 datalen;
  UINT64 bitlen;
  UINT32 state[8];
} sha256_ctx_t;

static void sha256_transform(sha256_ctx_t *ctx, const UINT8 *data) {
  UINT32 a,b,c,d,e,f,g,h,t1,t2,m[64];
  UINTN i, j;
  for (i = 0, j = 0; i < 16; ++i, j += 4)
    m[i] = ((UINT32)data[j] << 24) | ((UINT32)data[j+1] << 16) |
           ((UINT32)data[j+2] << 8)  | (UINT32)data[j+3];
  for (; i < 64; ++i)
    m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
  a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
  e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
  for (i = 0; i < 64; ++i) {
    t1 = h + EP1(e) + CH(e,f,g) + k[i] + m[i];
    t2 = EP0(a) + MAJ(a,b,c);
    h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
  ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx_t *ctx) {
  ctx->datalen = 0; ctx->bitlen = 0;
  ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
  ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
  ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
  ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}

static void sha256_update(sha256_ctx_t *ctx, const UINT8 *data, UINTN len) {
  for (UINTN i = 0; i < len; ++i) {
    ctx->data[ctx->datalen++] = data[i];
    if (ctx->datalen == 64) {
      sha256_transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}

static void sha256_final(sha256_ctx_t *ctx, UINT8 hash[32]) {
  UINT32 i = ctx->datalen;
  if (ctx->datalen < 56) {
    ctx->data[i++] = 0x80;
    while (i < 56) ctx->data[i++] = 0;
  } else {
    ctx->data[i++] = 0x80;
    while (i < 64) ctx->data[i++] = 0;
    sha256_transform(ctx, ctx->data);
    SetMem(ctx->data, 56, 0);
  }
  ctx->bitlen += (UINT64)ctx->datalen * 8;
  ctx->data[63] = (UINT8)ctx->bitlen;
  ctx->data[62] = (UINT8)(ctx->bitlen >> 8);
  ctx->data[61] = (UINT8)(ctx->bitlen >> 16);
  ctx->data[60] = (UINT8)(ctx->bitlen >> 24);
  ctx->data[59] = (UINT8)(ctx->bitlen >> 32);
  ctx->data[58] = (UINT8)(ctx->bitlen >> 40);
  ctx->data[57] = (UINT8)(ctx->bitlen >> 48);
  ctx->data[56] = (UINT8)(ctx->bitlen >> 56);
  sha256_transform(ctx, ctx->data);
  for (i = 0; i < 4; ++i) {
    hash[i]    = (UINT8)(ctx->state[0] >> (24 - i * 8));
    hash[i+4]  = (UINT8)(ctx->state[1] >> (24 - i * 8));
    hash[i+8]  = (UINT8)(ctx->state[2] >> (24 - i * 8));
    hash[i+12] = (UINT8)(ctx->state[3] >> (24 - i * 8));
    hash[i+16] = (UINT8)(ctx->state[4] >> (24 - i * 8));
    hash[i+20] = (UINT8)(ctx->state[5] >> (24 - i * 8));
    hash[i+24] = (UINT8)(ctx->state[6] >> (24 - i * 8));
    hash[i+28] = (UINT8)(ctx->state[7] >> (24 - i * 8));
  }
}

void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]) {
  sha256_ctx_t ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, buf, len);
  sha256_final(&ctx, out);
}
#endif
