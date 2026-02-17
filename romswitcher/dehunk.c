/*
 * dehunk: Amiga hunk-to-ROM converter.
 *
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2025.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <err.h>

#define DPRINTF(...) if (txtout != NULL) fprintf(txtout, __VA_ARGS__)

#define SWAP16(x) __builtin_bswap16(x)
#define SWAP32(x) __builtin_bswap32(x)
#define SWAP64(x) __builtin_bswap64(x)

#define HUNK_UNIT         999
#define HUNK_NAME         1000
#define HUNK_CODE         1001
#define HUNK_DATA         1002
#define HUNK_BSS          1003
#define HUNK_RELOC32      1004
#define HUNK_RELOC16      1005
#define HUNK_RELOC8       1006
#define HUNK_EXT          1007
#define HUNK_SYMBOL       1008
#define HUNK_DEBUG        1009
#define HUNK_END          1010
#define HUNK_HEADER       1011
#define HUNK_OVERLAY      1013
#define HUNK_BREAK        1014
#define HUNK_DREL32       1015
#define HUNK_DREL16       1016
#define HUNK_DREL8        1017
#define HUNK_LIB          1018
#define HUNK_INDEX        1019
#define HUNK_RELOC32SHORT 1020  // Same as 1015
#define HUNK_RELRELOC32   1021
#define HUNK_ABSRELOC16   1022

/*
 * HUNK_HEADER
 */
typedef struct {
    uint32_t hh_strings;    // List of resident library names (should be empty)
    uint32_t hh_table_size; // Highest hunk number plus one
    uint32_t hh_first_hunk; // The first hunk for loading
    uint32_t hh_last_hunk;  // The last hunk for loading
    uint32_t hh_sizes[0];   // A list of hunk sizes
} hunk_header_t;

/*
 * HUNK_UNIT
 */
typedef struct {
    uint32_t hu_size;       // Size in longwords
    uint32_t hu_data[0];    // Machine code
} hunk_unit_t;

/*
 * HUNK_CODE
 */
typedef struct {
    uint32_t hc_size;       // Size in longwords
    uint32_t hc_data[0];    // Machine code
} hunk_code_t;

/*
 * HUNK_DATA
 */
typedef struct {
    uint32_t hd_size;       // Size in longwords
    uint32_t hd_data[0];    // Data
} hunk_data_t;

/*
 * HUNK_DEBUG
 */
typedef struct {
    uint32_t hd_size;       // Size in longwords
    uint32_t hd_base_off;   // Base offset in source
    char     hd_hcln[4];    // "HCLN"
    uint32_t hd_fname_len;  // Source file name length in longwords
    uint32_t hd_data[0];    // Source file name and table of line offsets
} hunk_debug_t;

FILE *txtout = NULL;

static uint32_t *
skip_symbols(uint32_t *cur, uint32_t *bufend)
{
    uint32_t count;
    while (cur < bufend) {
        count = SWAP32(*cur);
        cur++;  // skip length of this symbol name
        if (count == 0)
            break;  // end of this block
#if 0
        DPRINTF("SYMBOL c=%u %08x\n", count, count);
#endif
        cur += count + 1;  // # dwords + symbol_offset
    }
    return (cur);
}

static uint32_t *
skip_debug(uint32_t *cur, uint32_t *bufend)
{
    uint32_t size;
    hunk_debug_t *hdr = (hunk_debug_t *) cur;
    size = SWAP32(hdr->hd_size);
#if 0
    DPRINTF("DEBUG size=%x hd_fname_len=%x\n",
            SWAP32(hdr->hd_size), SWAP32(hdr->hd_fname_len));
#endif
    return (cur + size + 1);
}

static void
usage(void)
{
    fprintf(stderr,
            "This program is used to convert an Amiga hunk file to ROM image.\n"
            "Usage: dehunk [-v] infile outfile\n"
            "-h  display help\n"
            "-v  verbose output\n");
}

int
main(int argc, char *argv[])
{
    int arg;
    const char *infile = NULL;
    const char *outfile = NULL;
    struct stat st;
    FILE *ifp;
    FILE *ofp;
    uint filesize;
    uint32_t *buf;
    uint32_t *bufend;
    uint32_t *cur;
    hunk_header_t *hh;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            for (ptr++; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'h':
                    case '?':
                        usage();
                        exit(0);
                    case 'v':
                        txtout = stdout;
                        break;
                    default:
                        printf("Unknown argument -%s\n", ptr);
                        usage();
                        exit(1);
                }
            }
        } else if (infile == NULL) {
            infile = ptr;
        } else if (outfile == NULL) {
            outfile = ptr;
        } else {
            fprintf(stderr, "Too many arguments: %s\n", argv[arg]);
        }
    }
    if (outfile == NULL) {
        errx(EXIT_FAILURE,
             "Not enough arguments. You must provide infile and outfile");
    }

    if (strcmp(outfile, "-") == 0) {
        txtout = stderr;
        ofp = stdout;
#if 0
    } else {
        /* Verify outfile doesn't exist */
        if (stat(outfile, &st) == 0)
            errx(EXIT_FAILURE, "output file %s already exists", outfile);
#endif
    }

    /* Get size of infile */
    if (stat(infile, &st) != 0)
        err(EXIT_FAILURE, "stat(%s) fail", infile);

    DPRINTF("size %u\n", st.st_size);
    filesize = st.st_size;

    buf = malloc(filesize);
    if (buf == NULL)
        err(EXIT_FAILURE, "Failed to allocate %u bytes", filesize);

    if ((ifp = fopen(infile, "r")) == NULL)
        err(EXIT_FAILURE, "fopen(%s) for input fail", infile);

    if (fread(buf, filesize, 1, ifp) != 1)
        err(EXIT_FAILURE, "Failed to read %u bytes of %s", filesize, infile);

    /* Should find HUNK_HEADER */
    DPRINTF("%u %x\n", SWAP32(buf[0]), buf[0]);
    if (buf[0] != SWAP32(HUNK_HEADER)) {
        errx(EXIT_FAILURE, "Failed to find hunk header %u at offset 0; "
             "got 0x08x\n", SWAP32(buf[0]), HUNK_HEADER);
    }

    /*
     * Example file 1
     * --------------
     * 00000000: 000003f3 00000000 0000000e 00000000
     *           ^HUNK_HEADER
     * 00000010: 0000000d 00002ca4 00000000 00000522
     * 00000020: 0000d31d 40000006 0000014c 000004c6
     * 00000030: 000001bc 0000002e 00000760 00000277
     * 00000040: 00000126 00000559 00000059 000003e9
     *                             ^lastsiz ^HUNK_CODE
     * 00000050: 00002ca4 11144ef9 00f80010 4e714e75
     *                    ^start of ROM
     *
     * Example file 2
     * --------------
     * 00000000: 000003f3 00000000 0000000d 00000000
     * 00000010: 0000000c 00002c87 00000000 00000522
     * 00000020: 0000d33a 0000014c 000004c6 000001bc
     * 00000030: 0000002e 00000760 00000277 00000126
     * 00000040: 00000559 00000059 000003e9 00002c87
     *                             ^HUNK_CODE
     * 00000050: 11144ef9 00f80010 4e714e75 00000000
     *           ^^start of ROM
     */
    hh = (hunk_header_t *) (buf + 1);
    uint hsize;
    uint hunknum;
    DPRINTF("Header table_size=%u first_hunk=%u last_hunk=%u\n",
            SWAP32(hh->hh_table_size),
            SWAP32(hh->hh_first_hunk), SWAP32(hh->hh_last_hunk));
    uint hunks = SWAP32(hh->hh_last_hunk) - SWAP32(hh->hh_first_hunk) + 1;
    if (hunks > 16) {
        fprintf(stderr, "Strange number of hunks: %u\n", hunks);
        exit(1);
    }
    cur = (uint32_t *) &(hh->hh_sizes[hunks]);
    bufend = (uint32_t *) ((uintptr_t) buf + filesize);
    if ((ofp = fopen(outfile, "w")) == NULL)
        err(EXIT_FAILURE, "fopen(%s) for output fail", infile);
    hunknum = 0;
    while (cur < bufend) {
        uint32_t      hsize = SWAP32(hh->hh_sizes[hunknum]);
        uint          hunktype = SWAP32(*cur);
        const char   *hunkname;
        hunk_data_t  *hdata;
        hunk_code_t  *hcode;
        uint32_t     *data;
        uint          lwords = 0;
        uint          show = 1;

        hunktype = SWAP32(*cur);
        switch (hunktype) {
            case HUNK_CODE:
                hunkname = "CODE";
                hcode = (hunk_code_t *) (cur + 1);
                lwords = SWAP32(hcode->hc_size);
                data = (uint32_t *) &(hcode->hc_data[0]);
                if (fwrite(data, lwords * sizeof (uint32_t), 1, ofp) != 1) {
                    err(EXIT_FAILURE, "Failed to write %u bytes\n",
                        lwords * sizeof (uint32_t));
                }
                cur = data + lwords;
                break;
            case HUNK_DATA:
                hunkname = "DATA";
                hdata = (hunk_data_t *) (cur + 1);
                lwords = SWAP32(hdata->hd_size);
                data = (uint32_t *) &(hdata->hd_data[0]);
                if (fwrite(data, lwords * sizeof (uint32_t), 1, ofp) != 1) {
                    err(EXIT_FAILURE, "Failed to write %u bytes\n",
                        lwords * sizeof (uint32_t));
                }
                cur = data + lwords;
                break;
            case HUNK_SYMBOL:
                hunkname = "SYMBOL";
                data = skip_symbols(cur + 1, bufend);
                lwords = data - cur;
                cur = data;
                break;
            case HUNK_DEBUG:
                hunkname = "DEBUG";
                data = skip_debug(cur + 1, bufend);
                lwords = data - cur;
                cur = data;
                break;
            case HUNK_BSS:
                /* Don't need BSS in ROM file */
                hunkname = "BSS";
                cur++;
                lwords = SWAP32(*cur);
                cur++;
                show = 0;
                break;
            case HUNK_END:
                hunkname = "END";
                cur++;
                break;
            default:
                fprintf(stderr, "\nUnknown Hunk type %u (0x%x) at 0x%x\n",
                        hunktype, hunktype,
                        (uintptr_t) cur - (uintptr_t) buf);
                fprintf(stderr, "[%08x %08x %08x %08x]\n",
                        SWAP32(cur[0]), SWAP32(cur[1]), SWAP32(cur[2]),
                        SWAP32(cur[3]));
                exit(1);
        }
        if (show) {
            DPRINTF("%-6s (%u) len=%u\n", hunkname, hunktype,
                    lwords * sizeof (uint32_t));
        }
    }

    if (ofp != stdout)
        fclose(ofp);
fail_read:
    free(buf);
    fclose(ifp);
    return (0);
}
