/* tools/mode2-profile/mode2-profile.c — C mode-2 profile tool.
   derive: vbmeta.img -> profile.toml   (Task 3)
   compile: profile.toml -> 120-byte gbl_mode2_profile binary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vendor/tomlc99/toml.h"
#include "../shared/gbl_mode2_profile.h"

static void wle16(uint8_t *p, uint16_t v){p[0]=v;p[1]=v>>8;}
static void wle32(uint8_t *p, uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

/* fail: print "error: ..." to stderr and exit 1. No partial output. */
static void fail(const char *msg){ fprintf(stderr,"error: %s\n",msg); exit(1); }

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
    if (!f) { perror(in); return 1; }
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

    if (intkey(t,"version",1,1) != 1) fail("version must be 1");
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
    if (!o) { perror(out); return 1; }
    if (fwrite(b,1,sizeof b,o)!=sizeof b){ fclose(o);
        fprintf(stderr,"error: write failed\n"); return 1; }
    fclose(o);
    fprintf(stderr,"wrote %s (%u bytes)\n", out, (unsigned)sizeof b);
    return 0;
}

int derive_main(int argc, char **argv);  /* Task 3, in this file */

/* Stub so the tool links before Task 3 implements derive_main. */
int derive_main(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr,"error: derive not yet implemented\n");
    return 2;
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
