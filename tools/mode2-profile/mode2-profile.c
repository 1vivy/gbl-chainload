/* tools/mode2-profile/mode2-profile.c — C mode-2 profile tool.
   derive: vbmeta.img -> profile.toml   (Task 3)
   compile: profile.toml -> 120-byte gbl_mode2_profile binary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include "vendor/tomlc99/toml.h"
#include "../shared/gbl_mode2_profile.h"
#include "Internal/Sha256.h"

static void wle16(uint8_t *p, uint16_t v){p[0]=v;p[1]=v>>8;}
static void wle32(uint8_t *p, uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

/* hexkey: read a TOML string key, require exactly 64 lowercase-hex, decode
   into out[32]. */
static void hexkey(toml_table_t *t, const char *key, uint8_t out[32]) {
    toml_datum_t d = toml_string_in(t, key);
    if (!d.ok) { fprintf(stderr,"error: '%s' missing or not a string\n",key); exit(1); }
    if (strlen(d.u.s) != 64) { free(d.u.s);
        fprintf(stderr,"error: '%s' must be 64 hex chars\n",key); exit(1); }
    for (int i=0;i<32;i++){
        int hi=d.u.s[2*i], lo=d.u.s[2*i+1];
        if(!( (hi>='0'&&hi<='9')||(hi>='a'&&hi<='f') ) ||
           !( (lo>='0'&&lo<='9')||(lo>='a'&&lo<='f') )) { free(d.u.s);
            fprintf(stderr,"error: '%s' contains non-lowercase-hex\n",key); exit(1); }
        out[i] = (uint8_t)((( hi<='9'?hi-'0':hi-'a'+10)<<4)|(lo<='9'?lo-'0':lo-'a'+10));
    }
    free(d.u.s);
}

/* intkey: read a TOML integer key, enforce [lo,hi]. */
static int64_t intkey(toml_table_t *t, const char *key, int64_t lo, int64_t hi) {
    toml_datum_t d = toml_int_in(t, key);
    if (!d.ok) { fprintf(stderr,"error: '%s' missing or not an integer\n",key); exit(1); }
    if (d.u.i < lo || d.u.i > hi) {
        fprintf(stderr,"error: '%s' out of range %lld..%lld (got %lld)\n",
                key,(long long)lo,(long long)hi,(long long)d.u.i); exit(1); }
    return d.u.i;
}

static int do_compile(const char *in, const char *out) {
    FILE *f = fopen(in,"r");
    if (!f) { fprintf(stderr,"error: cannot open %s: %s\n",in,strerror(errno)); return 1; }
    char errbuf[200];
    toml_table_t *t = toml_parse_file(f, errbuf, sizeof errbuf);
    fclose(f);
    if (!t) { fprintf(stderr,"error: malformed profile TOML: %s\n",errbuf); return 1; }

    /* reject unknown keys */
    static const char *known[] = {"version","is_unlocked","color","system_version",
        "system_spl","rot_digest","pubkey_digest","vbh"};
    for (int i=0;; i++) {
        const char *k = toml_key_in(t, i);
        if (!k) break;
        int ok=0; for (unsigned j=0;j<sizeof known/sizeof*known;j++)
            if(!strcmp(k,known[j])) ok=1;
        if(!ok){ fprintf(stderr,"error: unknown key '%s' in profile\n",k);
                 toml_free(t); return 1; }
    }

    intkey(t,"version",1,1);
    uint32_t is_unlocked    = (uint32_t)intkey(t,"is_unlocked",0,1);
    uint32_t color          = (uint32_t)intkey(t,"color",0,3);
    uint32_t system_version = (uint32_t)intkey(t,"system_version",0,0xFFFFFFFFLL);
    uint32_t system_spl     = (uint32_t)intkey(t,"system_spl",0,0xFFFFFFFFLL);
    uint8_t rot[32], pk[32], vbh[32];
    hexkey(t,"rot_digest",rot);
    hexkey(t,"pubkey_digest",pk);
    hexkey(t,"vbh",vbh);
    toml_free(t);

    uint8_t b[GBL_M2P_SIZE];
    memset(b,0,sizeof b);
    memcpy(b+0, GBL_M2P_MAGIC, 4);
    wle16(b+4, GBL_M2P_VERSION);
    /* b+6 reserved = 0 */
    wle32(b+8,  is_unlocked);
    wle32(b+12, color);
    wle32(b+16, system_version);
    wle32(b+20, system_spl);
    memcpy(b+24, rot, 32);
    memcpy(b+56, pk,  32);
    memcpy(b+88, vbh, 32);

    FILE *o = fopen(out,"wb");
    if (!o) { fprintf(stderr,"error: cannot open %s: %s\n",out,strerror(errno)); return 1; }
    if (fwrite(b,1,sizeof b,o)!=sizeof b){
        fclose(o); remove(out);
        fprintf(stderr,"error: write failed\n"); return 1; }
    fclose(o);
    fprintf(stdout,"wrote %s (%u bytes)\n", out, (unsigned)sizeof b);
    return 0;
}

/* ---- AVB vbmeta header field byte offsets (big-endian, from avbtool.py
   AvbVBMetaHeader format string — cross-verified against the 256-byte struct)
   magic@0(4), req_maj@4(4), req_min@8(4),
   auth_size@12(8), aux_size@20(8), alg_type@28(4),
   hash_off@32(8), hash_size@40(8), sig_off@48(8), sig_size@56(8),
   pk_off@64(8),  pk_size@72(8), pkm_off@80(8), pkm_size@88(8),
   desc_off@96(8), desc_size@104(8), rollback@112(8),
   flags@120(4), rollback_loc@124(4), release@128(48), padding(80) = 256 */

#define AVB_HDR_SIZE         256u
#define AVB_AUTH_SIZE_OFF     12u
#define AVB_AUX_SIZE_OFF      20u
#define AVB_PK_OFF_OFF        64u
#define AVB_PK_SIZE_OFF       72u
#define AVB_DESC_OFF_OFF      96u
#define AVB_DESC_SIZE_OFF    104u

static uint64_t rbe64(const uint8_t *p) {
    return ((uint64_t)p[0]<<56)|((uint64_t)p[1]<<48)|
           ((uint64_t)p[2]<<40)|((uint64_t)p[3]<<32)|
           ((uint64_t)p[4]<<24)|((uint64_t)p[5]<<16)|
           ((uint64_t)p[6]<< 8)| (uint64_t)p[7];
}

static void hex64(const uint8_t digest[32], char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2*i]   = h[digest[i] >> 4];
        out[2*i+1] = h[digest[i] & 0xf];
    }
    out[64] = '\0';
}

/*
 * derive_main — AVB vbmeta reader; writes a TOML profile byte-identical to
 * the Python mode2-profile.py cmd_derive output.
 *
 * Usage: mode2-profile derive <vbmeta.img> -o <out.toml>
 */
int derive_main(int argc, char **argv) {
    /* argv: [0]=prog [1]="derive" [2]=vbmeta [3]="-o" [4]=output */
    if (argc < 5 || strcmp(argv[3], "-o") != 0) {
        fprintf(stderr,
            "usage: mode2-profile derive <vbmeta.img> -o <out.toml>\n");
        return 2;
    }
    const char *vbmeta_path = argv[2];
    const char *out_path    = argv[4];

    /* --- read entire file into memory --- */
    FILE *fv = fopen(vbmeta_path, "rb");
    if (!fv) { fprintf(stderr,"error: cannot open %s: %s\n",vbmeta_path,strerror(errno)); return 1; }
    if (fseek(fv, 0, SEEK_END) != 0) { fprintf(stderr,"error: fseek: %s\n",strerror(errno)); fclose(fv); return 1; }
    long fsz = ftell(fv);
    if (fsz < 0) { fprintf(stderr,"error: ftell: %s\n",strerror(errno)); fclose(fv); return 1; }
    rewind(fv);
    if ((size_t)fsz < AVB_HDR_SIZE) {
        fprintf(stderr, "error: %s: too small to be a vbmeta image\n",
                vbmeta_path);
        fclose(fv); return 1;
    }
    uint8_t *img = (uint8_t *)malloc((size_t)fsz);
    if (!img) { fprintf(stderr,"error: out of memory\n"); fclose(fv); return 1; }
    if (fread(img, 1, (size_t)fsz, fv) != (size_t)fsz) {
        fprintf(stderr,"error: read failed\n"); free(img); fclose(fv); return 1;
    }
    fclose(fv);

    /* --- verify magic --- */
    if (memcmp(img, "AVB0", 4) != 0) {
        fprintf(stderr,"error: %s: not a vbmeta image (bad magic)\n",
                vbmeta_path);
        free(img); return 1;
    }

    /* --- parse header --- */
    uint64_t auth_size  = rbe64(img + AVB_AUTH_SIZE_OFF);
    uint64_t aux_size   = rbe64(img + AVB_AUX_SIZE_OFF);
    uint64_t pk_off     = rbe64(img + AVB_PK_OFF_OFF);   /* within aux */
    uint64_t pk_size    = rbe64(img + AVB_PK_SIZE_OFF);
    uint64_t desc_off   = rbe64(img + AVB_DESC_OFF_OFF); /* within aux */
    uint64_t desc_size  = rbe64(img + AVB_DESC_SIZE_OFF);

    if (pk_size == 0) {
        fprintf(stderr,"error: %s: vbmeta has no public key (unsigned?)\n",
                vbmeta_path);
        free(img); return 1;
    }

    /* Fix 1: overflow-safe bounds checks before any addition */
    if (auth_size > (uint64_t)fsz - AVB_HDR_SIZE) {
        fprintf(stderr,"error: %s: auth block extends past file\n",
                vbmeta_path);
        free(img); return 1;
    }
    uint64_t aux_off = AVB_HDR_SIZE + auth_size;
    if (aux_size > (uint64_t)fsz - aux_off) {
        fprintf(stderr,"error: %s: aux block extends past file\n",
                vbmeta_path);
        free(img); return 1;
    }
    if (pk_off > aux_size || pk_size > aux_size - pk_off) {
        fprintf(stderr,"error: %s: public key extends past aux block\n",
                vbmeta_path);
        free(img); return 1;
    }
    if (desc_off > aux_size || desc_size > aux_size - desc_off) {
        fprintf(stderr,"error: %s: descriptor region extends past aux block\n",
                vbmeta_path);
        free(img); return 1;
    }

    const uint8_t *pubkey = img + aux_off + pk_off;

    /* --- compute digests --- */
    /* rot_digest = SHA256(pubkey || 0x00) */
    uint8_t rot_digest[32], pubkey_digest[32], vbh_digest[32];
    {
        uint8_t *buf = (uint8_t *)malloc(pk_size + 1);
        if (!buf) { fprintf(stderr,"error: out of memory\n"); free(img); return 1; }
        memcpy(buf, pubkey, pk_size);
        buf[pk_size] = 0x00;
        gbl_sha256(buf, pk_size + 1, rot_digest);
        free(buf);
    }
    gbl_sha256(pubkey, (size_t)pk_size, pubkey_digest);

    /* vbh = SHA256(image[0 .. 256 + auth_size + aux_size])
       No overflow possible: auth_size and aux_size were already validated to
       fit within fsz - AVB_HDR_SIZE and fsz - aux_off respectively, so
       aux_off + aux_size <= fsz, and vbmeta_size == aux_off + aux_size. */
    uint64_t vbmeta_size = aux_off + aux_size;
    if (vbmeta_size > (uint64_t)fsz) {
        fprintf(stderr,"error: %s: vbmeta declares %llu bytes but file is only %ld\n",
                vbmeta_path, (unsigned long long)vbmeta_size, fsz);
        free(img); return 1;
    }
    gbl_sha256(img, (size_t)vbmeta_size, vbh_digest);

    /* sha256 of the whole file (for the provenance comment) */
    uint8_t src_sha_bytes[32];
    gbl_sha256(img, (size_t)fsz, src_sha_bytes);
    char src_sha[65]; hex64(src_sha_bytes, src_sha);

    /* --- walk property descriptors --- */
    const uint8_t *desc_data = img + aux_off + desc_off;
    uint64_t os_ver_encoded = 0, spl_encoded = 0;
    char os_ver_str[128] = {0}, spl_str[128] = {0};
    int found_os  = 0, found_spl = 0;

    uint64_t doff = 0;
    while (doff + 16 <= desc_size) {
        const uint8_t *d = desc_data + doff;
        uint64_t tag = rbe64(d);
        uint64_t nb  = rbe64(d + 8);  /* num_bytes_following (already padded) */

        /* check we don't read past desc_data */
        if (nb > desc_size - doff - 16) break;

        if (tag == 0) {
            /* property descriptor — body starts at d+16
               body layout: key_size(u64 BE), val_size(u64 BE), key\0val\0 */
            if (nb < 16) { doff += 16 + nb; continue; }
            uint64_t klen = rbe64(d + 16);
            uint64_t vlen = rbe64(d + 24);
            /* key starts at d+32, followed by \0, then value, then \0.
               Total descriptor span is 16+nb bytes (tag+nb header = 16, body = nb). */
            /* Fix 4: pre-check individual lengths before combining to prevent wrap */
            if (klen > nb || vlen > nb - klen) {
                doff += 16 + nb; continue;
            }
            if (klen + 1 + vlen + 1 > nb - 16u) {
                doff += 16 + nb; continue;
            }
            const char *key = (const char *)(d + 32);
            const char *val = (const char *)(d + 32 + klen + 1);

            if (klen == strlen("com.android.build.boot.os_version") &&
                memcmp(key, "com.android.build.boot.os_version", klen) == 0) {
                size_t vsz = vlen < sizeof(os_ver_str)-1 ? (size_t)vlen
                                                          : sizeof(os_ver_str)-1;
                memcpy(os_ver_str, val, vsz);
                os_ver_str[vsz] = '\0';
                found_os = 1;
            }
            if (klen == strlen("com.android.build.boot.security_patch") &&
                memcmp(key, "com.android.build.boot.security_patch", klen) == 0) {
                size_t vsz = vlen < sizeof(spl_str)-1 ? (size_t)vlen
                                                       : sizeof(spl_str)-1;
                memcpy(spl_str, val, vsz);
                spl_str[vsz] = '\0';
                found_spl = 1;
            }
        }
        doff += 16 + nb;
    }

    if (!found_os) {
        fprintf(stderr,
            "error: vbmeta has no com.android.build.boot.os_version property\n");
        free(img); return 1;
    }
    if (!found_spl) {
        fprintf(stderr,
            "error: vbmeta has no com.android.build.boot.security_patch property\n");
        free(img); return 1;
    }

    /* --- encode os_version --- */
    {
        int major=0, minor=0, sub=0;
        /* parse M.N.P — missing components default to 0 */
        const char *p = os_ver_str[0] ? os_ver_str : "0";
        /* sscanf handles "M", "M.N", "M.N.P" */
        sscanf(p, "%d.%d.%d", &major, &minor, &sub);
        /* Fix 3: match Python _encode_os_version range checks */
        if (minor < 0 || minor > 0x7F) {
            fprintf(stderr,"error: OS version minor %d exceeds 7-bit limit\n", minor);
            free(img); return 1;
        }
        if (sub < 0 || sub > 0x7F) {
            fprintf(stderr,"error: OS version sub %d exceeds 7-bit limit\n", sub);
            free(img); return 1;
        }
        if (major < 0 || major > 0x3FFFF) {
            fprintf(stderr,"error: OS version major %d exceeds 18-bit limit\n", major);
            free(img); return 1;
        }
        os_ver_encoded = ((uint64_t)major << 14) | ((uint64_t)minor << 7)
                         | (uint64_t)sub;
    }

    /* --- encode spl --- */
    {
        int year=0, month=0, day=0;
        if (sscanf(spl_str, "%d-%d-%d", &year, &month, &day) < 3) {
            fprintf(stderr,
                "error: unrecognized security patch %s (expected YYYY-MM-DD)\n",
                spl_str);
            free(img); return 1;
        }
        /* Fix 2: match Python _encode_spl range checks */
        if (year < 2000 || year > 2127) {
            fprintf(stderr,"error: SPL year %d out of range (2000-2127)\n", year);
            free(img); return 1;
        }
        if (month < 1 || month > 12) {
            fprintf(stderr,"error: SPL month %d out of range (1-12)\n", month);
            free(img); return 1;
        }
        if (day < 1 || day > 31) {
            fprintf(stderr,"error: SPL day %d out of range (1-31)\n", day);
            free(img); return 1;
        }
        spl_encoded = ((uint64_t)day << 11) | ((uint64_t)(year - 2000) << 4)
                      | (uint64_t)month;
    }

    /* hex-encode digests */
    char rot_hex[65], pk_hex[65], vbh_hex[65];
    hex64(rot_digest,   rot_hex);
    hex64(pubkey_digest, pk_hex);
    hex64(vbh_digest,   vbh_hex);

    free(img);

    /* --- write TOML (byte-identical layout to Python cmd_derive) ---
       Python uses repr(Path(vbmeta_path)) for the source comment, which
       renders as PosixPath('...') on Linux.
       Python uses {os_ver:x} / {spl:x} for the hex literal (no leading zeros,
       no 0x prefix in the comment — the comment format is:
         # os_version: 'ver_str' -> 0xXX   spl: 'spl_str' -> 0xXX
       and the TOML values are:
         system_version = 0xXX   (Python f"0x{os_ver:x}")
         system_spl     = 0xXX
    */
    FILE *fo = fopen(out_path, "w");
    if (!fo) { fprintf(stderr,"error: cannot open %s: %s\n",out_path,strerror(errno)); return 1; }

    int wok = fprintf(fo,
        "# generated by mode2-profile derive\n"
        "# source: PosixPath('%s')\n"
        "# sha256: %s\n"
        "# os_version: '%s' -> 0x%llx   spl: '%s' -> 0x%llx\n"
        "version        = 1\n"
        "is_unlocked    = 0\n"
        "color          = 0\n"
        "system_version = 0x%llx\n"
        "system_spl     = 0x%llx\n"
        "rot_digest     = \"%s\"\n"
        "pubkey_digest  = \"%s\"\n"
        "vbh            = \"%s\"\n",
        vbmeta_path,
        src_sha,
        os_ver_str, (unsigned long long)os_ver_encoded,
        spl_str,    (unsigned long long)spl_encoded,
        (unsigned long long)os_ver_encoded,
        (unsigned long long)spl_encoded,
        rot_hex, pk_hex, vbh_hex);
    /* Fix 5: detect write failures (full disk etc.) */
    if (wok < 0 || fclose(fo) != 0) {
        remove(out_path);
        fprintf(stderr,"error: write failed for %s\n", out_path);
        return 1;
    }

    fprintf(stdout, "wrote %s\n", out_path);
    fprintf(stdout, "  rot_digest    = %s\n", rot_hex);
    fprintf(stdout, "  pubkey_digest = %s\n", pk_hex);
    fprintf(stdout, "  vbh           = %s\n", vbh_hex);
    fprintf(stdout, "  os_version    = '%s' -> 0x%llx\n",
            os_ver_str, (unsigned long long)os_ver_encoded);
    fprintf(stdout, "  spl           = '%s' -> 0x%llx\n",
            spl_str,    (unsigned long long)spl_encoded);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1],"compile") && !strcmp(argv[3],"-o"))
        return do_compile(argv[2], argv[4]);
    if (argc >= 2 && !strcmp(argv[1],"derive"))
        return derive_main(argc, argv);
    fprintf(stderr,
      "usage: mode2-profile compile <in.toml> -o <out.bin>\n"
      "       mode2-profile derive  <vbmeta.img> -o <out.toml>\n");
    return 2;
}
