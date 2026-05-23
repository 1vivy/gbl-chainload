/* tools/mode2-profile/mode2-profile.c — C mode-2 profile tool.
   derive:  vbmeta.img -> profile.toml
   compile: profile.toml -> 120-byte gbl_mode2_profile binary.

   PR2 Task 5: parse/compile/derive moved to crates/mode2-profile-core
   (Rust). The `compile` path here is a thin wrapper around
   gbl_mode2_profile_compile() — read the input file, call into Rust,
   write the output. `derive` stays C-side because the captured TOML
   golden (tools/mode2-profile/tests/baseline.toml.golden) locks the
   exact textual format (PosixPath comment, raw os_version string, …)
   and the C tool already produces byte-identical output to the Python
   reference. Task 8 collapses both paths into the `gbl` multicall. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include "../shared/gbl_mode2_profile.h"
/* PR2 Task 4: gbl_sha256 moved into crates/gblp1 (Rust). */
#include "../../crates/gblp1/include/gblp1_ffi.h"
/* PR2 Task 5: gbl_mode2_profile_compile/parse moved into
 * crates/mode2-profile-core (Rust). */
#include "../../crates/mode2-profile-core/include/mode2_profile_ffi.h"
/* PR2 Task 7: AvbParseLib's C ABI now lives in the crates/avb-parse
 * FFI header — including it provides EDK2 type shims under
 * __HOST_BUILD__, the GBL_AVB_* struct/enum/magic definitions, the
 * AvbParse_* entry-point decls, AND the inline AvbReadU{32,64}Be
 * helpers we still call directly below. */
#include "../../crates/avb-parse/include/avb_parse_ffi.h"

static int do_compile(const char *in, const char *out) {
    FILE *f = fopen(in,"r");
    if (!f) { fprintf(stderr,"error: cannot open %s: %s\n",in,strerror(errno)); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr,"error: fseek %s: %s\n", in, strerror(errno)); fclose(f); return 1;
    }
    long fsz = ftell(f);
    if (fsz < 0) {
        fprintf(stderr,"error: ftell %s: %s\n", in, strerror(errno)); fclose(f); return 1;
    }
    rewind(f);
    /* +1 for the NUL terminator — gbl_mode2_profile_compile wants a
       C string, not a length-prefixed buffer. */
    char *toml_str = (char *)malloc((size_t)fsz + 1);
    if (!toml_str) {
        fprintf(stderr,"error: out of memory\n"); fclose(f); return 1;
    }
    if ((long)fread(toml_str, 1, (size_t)fsz, f) != fsz) {
        fprintf(stderr,"error: read %s failed\n", in); free(toml_str); fclose(f); return 1;
    }
    fclose(f);
    toml_str[fsz] = '\0';

    uint8_t b[GBL_M2P_SIZE];
    size_t out_size = 0;
    int rc = gbl_mode2_profile_compile(toml_str, b, &out_size);
    free(toml_str);
    if (rc != 0) {
        switch (rc) {
            case GBL_M2P_COMPILE_MALFORMED_TOML:
                fprintf(stderr,"error: malformed profile TOML\n"); break;
            case GBL_M2P_COMPILE_MISSING_OR_TYPE:
                fprintf(stderr,"error: missing key or wrong type in profile\n"); break;
            case GBL_M2P_COMPILE_OUT_OF_RANGE:
                fprintf(stderr,"error: integer key out of range in profile\n"); break;
            case GBL_M2P_COMPILE_BAD_DIGEST:
                fprintf(stderr,"error: digest field is not 64 lowercase-hex chars\n"); break;
            case GBL_M2P_COMPILE_UNKNOWN_KEY:
                fprintf(stderr,"error: unknown key in profile\n"); break;
            default:
                fprintf(stderr,"error: compile failed (status=%d)\n", rc); break;
        }
        return 1;
    }
    if (out_size != GBL_M2P_SIZE) {
        fprintf(stderr,"error: compile produced %zu bytes (expected %u)\n",
                out_size, (unsigned)GBL_M2P_SIZE);
        return 1;
    }

    FILE *o = fopen(out,"wb");
    if (!o) { fprintf(stderr,"error: cannot open %s: %s\n",out,strerror(errno)); return 1; }
    if (fwrite(b,1,sizeof b,o)!=sizeof b){
        fclose(o); remove(out);
        fprintf(stderr,"error: write failed\n"); return 1; }
    fclose(o);
    fprintf(stdout,"wrote %s (%u bytes)\n", out, (unsigned)sizeof b);
    return 0;
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
    if ((size_t)fsz < GBL_AVB_VBMETA_HEADER_SIZE) {
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

    /* --- parse vbmeta header via AvbParseLib --- */
    GBL_AVB_VBMETA_HEADER hdr;
    EFI_STATUS s = AvbParse_VbmetaHeader (img, (UINT64)fsz, &hdr);
    if (s == EFI_NOT_FOUND) {
        fprintf(stderr, "error: %s: not a vbmeta image (bad magic)\n",
                vbmeta_path);
        free(img); return 1;
    }
    if (s != EFI_SUCCESS) {
        fprintf(stderr, "error: %s: malformed vbmeta header\n", vbmeta_path);
        free(img); return 1;
    }

    uint64_t auth_size  = hdr.AuthenticationDataBlockSize;
    uint64_t aux_size   = hdr.AuxiliaryDataBlockSize;
    uint64_t pk_off     = hdr.PublicKeyOffset;
    uint64_t pk_size    = hdr.PublicKeySize;
    uint64_t desc_off   = hdr.DescriptorsOffset;
    uint64_t desc_size  = hdr.DescriptorsSize;

    if (pk_size == 0) {
        fprintf(stderr, "error: %s: vbmeta has no public key (unsigned?)\n",
                vbmeta_path);
        free(img); return 1;
    }
    /* AvbParse_VbmetaHeader already enforced header + auth + aux <= file size,
       so aux_off computation below cannot overflow. */
    uint64_t aux_off = (uint64_t)GBL_AVB_VBMETA_HEADER_SIZE + auth_size;
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

    /* vbh = SHA256(image[0 .. 256 + auth_size + aux_size]).
       AvbParse_VbmetaHeader already validated header + auth + aux <= fsz,
       so aux_off + aux_size is safely <= fsz. */
    uint64_t vbmeta_size = aux_off + aux_size;
    gbl_sha256(img, (size_t)vbmeta_size, vbh_digest);

    /* sha256 of the whole file (for the provenance comment) */
    uint8_t src_sha_bytes[32];
    gbl_sha256(img, (size_t)fsz, src_sha_bytes);
    char src_sha[65]; hex64(src_sha_bytes, src_sha);

    /* --- walk property descriptors via AvbParseLib --- */
    const uint8_t *aux_block = img + aux_off;
    uint64_t os_ver_encoded = 0, spl_encoded = 0;
    char os_ver_str[128] = {0}, spl_str[128] = {0};
    int found_os = 0, found_spl = 0;

    UINT64 cursor = desc_off;
    UINT64 walk_end = desc_off + desc_size;
    while (cursor < walk_end) {
        GBL_AVB_DESCRIPTOR_TAG tag;
        const UINT8 *desc;
        UINT64 desc_len;
        EFI_STATUS ds = AvbParse_NextDescriptor (aux_block, walk_end,
                                                 &cursor, &tag, &desc, &desc_len);
        if (ds == EFI_END_OF_MEDIA) break;
        if (ds != EFI_SUCCESS) break; /* malformed — stop walking, do not abort */
        if (tag != GblAvbDescPropertyTag) continue;

        /* Property descriptor body layout (libavb): after the 16-byte header,
           key_size(u64 BE) at +16, val_size(u64 BE) at +24, then key\0val\0. */
        if (desc_len < 32) continue;
        uint64_t klen = AvbReadU64Be (desc + 16);
        uint64_t vlen = AvbReadU64Be (desc + 24);
        /* Overflow-safe bounds checks. */
        if (klen > desc_len - 32 || vlen > desc_len - 32 - klen) continue;
        if (klen + 1 + vlen + 1 > desc_len - 32) continue;

        const char *key = (const char *)(desc + 32);
        const char *val = (const char *)(desc + 32 + klen + 1);

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
    if (argc >= 2 && !strcmp(argv[1], "--version")) {
        printf("mode2-profile %s\n", GBL_TOOL_VERSION);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1],"compile") && !strcmp(argv[3],"-o"))
        return do_compile(argv[2], argv[4]);
    if (argc >= 2 && !strcmp(argv[1],"derive"))
        return derive_main(argc, argv);
    fprintf(stderr,
      "mode2-profile %s\n"
      "usage: mode2-profile compile <in.toml> -o <out.bin>\n"
      "       mode2-profile derive  <vbmeta.img> -o <out.toml>\n",
      GBL_TOOL_VERSION);
    return 2;
}
