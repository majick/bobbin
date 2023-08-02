#include "bobbin-internal.h"

/* For the ROM file, and error handling */
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Enough of a RAM buffer to provide 128k
//  Note: memory at 0xC000 thru 0xCFFF, and 0x1C000 thru 0x1CFFF,
//  are read at alternate banks of RAM for locations $D000 - $DFFF
static byte membuf[128 * 1024];

bool lc_prewrite;
bool lc_write = true;
               // ^ note that the value of this is the opposite of the real
               // hardware flip-flop... but using write == true to mean
               // you _can't_ write would be confusing...
bool lc_bank_one = false;
bool lc_read_bsr = false;

// Pointer to firmware, mapped into the Apple starting at $D000
static unsigned char *rombuf;
static unsigned char *ramloadbuf;
static size_t        ramloadsz;

static const char * const rom_dirs[] = {
    "BOBBIN_ROMDIR", // not a dirname, an env var name
    "./roms",        // also not a dirname; we'll use bobbin's dir instead
    ROMSRCHDIR,
};
static const char * const *romdirp = rom_dirs;
static const char * const * const romdend = rom_dirs + (sizeof rom_dirs)/(sizeof rom_dirs[0]);
static const char *get_try_rom_path(const char *fname) {
    static char buf[256];
    const char *env;
    const char *dir;

    if (romdirp == &rom_dirs[0] && (env = getenv(*romdirp++)) != NULL) {
        dir = env;
    } else if (romdirp == &rom_dirs[1]) {
        ++romdirp; // for next call to get_try_rom_path()

        // Check where bobbin's path from argv[0] is
        char *slash = strrchr(program_name, '/');
        if (slash == NULL) {
            dir = "."; // fallback to cwd
        } else {
            if ((slash - program_name) > ((sizeof buf)-1))
                goto realdir;
            dir = buf;
            size_t dirlen = slash - program_name;
            memcpy(buf, program_name, dirlen);
            (void) snprintf(&buf[dirlen], ((sizeof buf) - dirlen), "/roms");
        }
    } else {
realdir:
        if (romdirp != romdend) {
            dir = *romdirp++;
        }
        else {
            return NULL;
        }
    }
    VERBOSE("Looking for ROM named \"%s\" in %s...\n", fname, dir);
    if (dir != buf) {
        (void) snprintf(buf, sizeof buf, "%s/%s", dir, fname);
    } else {
        char *end = strchr(buf, 0);
        (void) snprintf(end, (sizeof buf) - (end - buf), "/%s", fname);
    }
    return buf;
}

static void load_rom(void)
{
    /* Load a 12k rom file into place (not provided; get your own!) */
    const char *rompath;
    const char *last_tried_path;
    int err = 0;
    int fd = -1;

    if (cfg.rom_load_file) {
        errno = 0;
        VERBOSE("Loading user rom file \"%s\"\n", cfg.rom_load_file);
        fd = open(cfg.rom_load_file, O_RDONLY);
        if (fd < 0) {
            DIE(1, "Couldn't open ROM file \"%s\": %s\n", cfg.rom_load_file,
                strerror(err));
        }
    } else {
        while (fd < 0
                && (rompath = get_try_rom_path(default_romfname)) != NULL) {
            errno = 0;
            last_tried_path = rompath;
            fd = open(rompath, O_RDONLY);
            err = errno;
        }
        if (fd < 0) {
            DIE(1, "Couldn't open ROM file \"%s\": %s\n", last_tried_path, strerror(err));
        } else {
            INFO("FOUND ROM file \"%s\".\n", rompath);
        }
    }

    struct stat st;
    errno = 0;
    err = fstat(fd, &st);
    if (err < 0) {
        err = errno;
        DIE(1, "Couldn't stat ROM file \"%s\": %s\n", last_tried_path, strerror(err));
    }
    if (st.st_size != 12 * 1024) {
        // XXX expected size will need to be configurable
        DIE(0, "ROM file \"%s\" has unexpected file size %zu\n",
            last_tried_path, (size_t)st.st_size);
        DIE(1, "  (expected %zu).\n",
            (size_t)(12 * 1024));
    }

    errno = 0;
    rombuf = mmap(NULL, 12 * 1024, PROT_READ, MAP_PRIVATE, fd, 0);
    if (rombuf == NULL) {
        DIE(1, "Couldn't map in ROM file: %s\n", strerror(errno));
    }
    close(fd); // safe to close now.

    if (!cfg.rom_load_file)
        validate_rom(rombuf, 12 * 1024);
}

bool check_asoft_link(unsigned char *buf, size_t start, size_t sz,
                      word w, long *chlenp)
{
    long chlen;
    bool val = true;
    for (chlen = 0; chlen < 65536; ++chlen) {
        if (w == 0x0000) {
            val = true;
            break;
        } else if (w < start || w > start + sz - sizeof(word)) {
            val = false;
            break;
        } else {
            // next link
            w = WORD(buf[w - start], buf[w - start + 1]);
        }
    }
    if (chlen == 65536)
        chlen = -1;
    if (val)
        *chlenp = chlen;
    return val;
}

static void adjust_asoft_start(unsigned char **bufp, size_t *szp)
{
    /*
       If we're loading an AppleSoft BASIC file, there is some
       possibility that the first two bytes are not actually part of
       the AppleSoft program to load, but are a word representing
       the size of the file, minus one (= size of program, plus one?)
       Apparently this is how Apple DOS stores the file, and there
       are tokenizer programs that output this way.

       In every case where we determine tha word 0 is a file length and
       not the first BASIC link, we complain with a warning to avoid
       tokenizers that prefix a file length header word.

       If word zero is not the file length, word zero is the first BASIC
       link. But check it anyway, and error if it's invalid.

       Otheriwse, if word zero is not a valid BASIC link, then we assume
       it's a file length and skip it.

       Otherwise, if word zero is a valid BASIC link, and word one is not,
       we assume word zero is the first BASIC link (and word one is
       the first line number).

       If both word zero and word one are valid BASIC links, we
       use word zero, UNLESS it appears to have a loop in the chain
       while word one doesn't, in which case we use word one.

       The notion of a "valid BASIC link" is determined as:
         (a) it refers to a memory location that resides within the
             loaded file's range, or else has the value zero.
         (b) if it refers to a memory location, the value at that
             location must itself a valid BASIC link (this definition
             is recursive)
       Chain linkage is only followed up to 65,535 links. If linkage
       exceeds this number (may be a looping chain), it is still
       considered a "valid BASIC link", but the chain length is reported
       as -1, for the purposes of comparing chain lengths (see above, in
       the case where both words zero and one are "valid BASIC links").

       Note that things like backwards-links, or links that jump forward
       by more than 256 bytes, or line numbers whose values jump around,
       are not checked (even though it is not normally possible to
       create such programs), because people do pull shenanigans with
       the BASIC program representation, and we want to be as permissive
       as possible.

       It is possible, though unlikely, for a value to be both a valid
       file length, *and* a valid BASIC link, if the file length is
       greater than the --load-at location (or default $801), and the
       value of that file length as a memory pointer just so happens to
       fall at a valid link.

       It is similarly possible, though unlikely, for a value to be both
       a valid line number, and a valid BASIC link.
    */
    if (*szp < sizeof (word)) {
        DIE(0, "--load-basic-bin: file \"%s\" has insufficient length (%zu)\n",
            cfg.ram_load_file, *szp);
        DIE(2, "  to be a valid AppleSoft BASIC program binary.\n");
    }

    INFO("Determining whether AppleSoft program in file \"%s\" begins\n",
         cfg.ram_load_file);
    INFO("  at the first word, or the second.\n");

    bool adjusted = false;
    const word lenval = (*szp)-1;
    word w0 = WORD((*bufp)[0], (*bufp)[1]);
    long w0chain;
    bool w0valid = check_asoft_link(*bufp, cfg.ram_load_loc, *szp,
                                    w0, &w0chain);
#define VPFX "    ..."
    if (w0valid) {
        VERBOSE(VPFX "word 0 is a valid BASIC chain of length %ld.\n",
                w0chain);
    } else {
        VERBOSE(VPFX "word 0 is not a valid BASIC chain.\n");
    }

    bool is_lenval = (w0 == lenval);
    VERBOSE(VPFX "word 0 is%s a valid file length.\n", is_lenval? "" : " not");
    if (is_lenval) {
        // Nothing to do yet, futher checking is required.
    } else if (!w0valid) {
        // Not the length value, but also not a valid BASIC link. Error.
        DIE(0, "First word of --load-basic-bin file \"%s\" is neither a\n",
            cfg.ram_load_file);
        DIE(0, "  valid filesize value, nor a valid AppleSoft link!\n");
        DIE(2, "  Not a valud AppleSoft BASIC program binary.\n");
    } else {
        // is not the length value, but is a BASIC link. We have enough info.
        goto no_adjust;
    }

    // From here on out, we know word zero was a valid file length!

    long w1chain;
    bool w1valid =
        (*szp > 2 * sizeof (word)) // don't read word 1 if it doesn't exist
        && check_asoft_link(*bufp + sizeof (word), cfg.ram_load_loc,
                            *szp - sizeof (word),
                            WORD((*bufp)[2], (*bufp)[3]), // w1
                            &w1chain);
    if (w1valid) {
        VERBOSE(VPFX "word 1 is a valid BASIC chain of length %ld.\n",
                w1chain);
    } else {
        VERBOSE(VPFX "word 1 is not a valid BASIC chain.\n");
    }

    if (w0valid) {
        // handled further below
    } else if (!w1valid) {
        DIE(0, "Neither word 0 nor word 1 are valid AppleSoft BASIC links.\n");
        DIE(0, "--load-basic-bin file \"%s\" is not an AppleSoft BASIC\n",
            cfg.ram_load_file);
        DIE(2, "  program binary.\n");
    } else {
        goto adjust; // word 0 is the length value, is not a valid link.
    }

    // Okay, both word 0 and word 1 are vaild BASIC links...
    WARN("AMBIGUOUS BASIC FILE! Word 0 and word 1 of --load-basic-bin\n");
    WARN("  file \"%s\" are BOTH valid program starts, but word 0\n",
         cfg.ram_load_file);
    WARN("  could ALSO be a valid file length header.\n");
    WARN("If the program listing appears to be wrong, please use a\n");
    WARN("  different AppleSoft tokenizer program\n");
    WARN("  (such as `bobbin --tokenize`), and start with a line\n");
    WARN("  number below 2000.\n");
    if (w0chain == -1 && w1chain != -1) {
        WARN("Using word 1, as word 0 appears to be an infinite chain.\n");
        goto adjust;
    }
    goto no_adjust;

#undef VPFX
adjust:
    WARN("file-length prefix detected for AppleSoft binary\n");
    WARN("  file \"%s\". Please use a tokenizer\n",
         cfg.ram_load_file);
    WARN("  that doesn't write this prefix (such as bobbin --tokenize.\n");
    adjusted = true;
    *bufp += 2;
    *szp -= 2;
    // fall through
no_adjust:
    INFO("Determination: AppleSoft program begins at word %s.\n",
         adjusted? "one" : "zero");
}

void load_ram_finish(void)
{
    unsigned char *buf = ramloadbuf;
    size_t sz = ramloadsz;

    if (cfg.basic_fixup) {
        adjust_asoft_start(&buf, &sz);
    }

    memcpy(&membuf[cfg.ram_load_loc], buf, sz);

    INFO("%zu bytes loaded into RAM from file \"%s\",\n",
         ramloadsz, cfg.ram_load_file);
    INFO("  starting at memory location $%04X.\n",
         (unsigned int)cfg.ram_load_loc);
    // Warning, this ^ is a lie for some values ($C000 - $CFFF), and
    //  aux mem locations

    if (cfg.basic_fixup) {
        // This was an AppleSoft BASIC file. Fixup some
        // zero-page values.
        poke_sneaky(ZP_TXTTAB, LO(cfg.ram_load_loc)   /* s/b $01 */);
        poke_sneaky(ZP_TXTTAB+1, HI(cfg.ram_load_loc) /* s/b $08 */);
        byte lo = LO(cfg.ram_load_loc + ramloadsz);
        byte hi = HI(cfg.ram_load_loc + ramloadsz);
        poke_sneaky(ZP_VARTAB, lo);
        poke_sneaky(ZP_VARTAB+1, hi);
        poke_sneaky(ZP_ARYTAB, lo);
        poke_sneaky(ZP_ARYTAB+1, hi);
        poke_sneaky(ZP_STREND, lo);
        poke_sneaky(ZP_STREND+1, hi);
        INFO("--load-basic-bin: AppleSoft settings adjusted.\n");
        VERBOSE("BASIC program start = $%X, end = $%X.\n",
                (unsigned int)(cfg.ram_load_loc),
                (unsigned int)WORD(lo, hi));
    }
}

static void load_ram(void)
{
    errno = 0;
    int fd = open(cfg.ram_load_file, O_RDONLY);
    if (fd < 0) {
        DIE(1, "Couldn't open --load file \"%s\": %s\n",
            cfg.ram_load_file, strerror(errno));
    }

    struct stat st;
    errno = 0;
    int err = fstat(fd, &st);
    if (err < 0) {
        err = errno;
        DIE(1, "Couldn't stat --load file \"%s\": %s\n", cfg.ram_load_file, strerror(err));
    }
    ramloadsz = st.st_size;

    if ((ramloadsz + cfg.ram_load_loc) > (sizeof membuf)) {
        DIE(1, "--load file \"%s\" would exceed the end of memory!\n",
            cfg.ram_load_file);
    }

    errno = 0;
    ramloadbuf = mmap(NULL, ramloadsz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ramloadbuf == NULL) {
        DIE(1, "Couldn't map in --load file: %s\n", strerror(errno));
    }
    close(fd); // safe to close now.

    if (cfg.delay_set) {
        INFO("--load file \"%s\" succeeded; delaying as requested.\n",
             cfg.ram_load_file);
    } else {
        load_ram_finish();
    }
}

static void fillmem(void)
{
    /* Immitate the on-boot memory pattern. */
    for (size_t z=0; z != sizeof membuf; ++z) {
        if (!(z & 0x2))
            membuf[z] = 0xFF;
    }

    /* Seed random generator */
    struct timespec tm;
    clock_gettime(CLOCK_REALTIME, &tm);
    srandom(tm.tv_nsec);

    /* For now at least, do what AppleWin does, and
     * set specific bytes to garbage. */
    for (size_t z=0; z < sizeof membuf; z += 0x200) {
        membuf[z + 0x28] = random();
        membuf[z + 0x29] = random();
        membuf[z + 0x68] = random();
        membuf[z + 0x69] = random();
    }
}

void mem_init_langcard(void)
{
    lc_prewrite = false;
    lc_write = true;
    lc_bank_one = false;
    lc_read_bsr = false;
}

void mem_init(void)
{
    if (cfg.ram_load_loc >= sizeof membuf) {
        DIE(2, "--load-at value must be less than $20000 (128k).\n");
    }

    fillmem();

    if (cfg.load_rom) {
        load_rom();
    } else if (cfg.ram_load_file == NULL) {
        DIE(2, "--no-rom given, but not --load!\n");
    }

    if (cfg.ram_load_file != NULL) {
        load_ram();
    }

    mem_init_langcard();
}

void mem_reboot(void)
{
    fillmem();

    if (cfg.ram_load_file != NULL) {
        if (ramloadbuf != NULL) {
            munmap(ramloadbuf, ramloadsz);
        }
        load_ram();
    }

    mem_init_langcard();
}

static int maybe_language_card(word loc, bool write)
{
    if ((loc & 0xFFF0) != SS_LANG_CARD) return -1;
    if (write)
        lc_prewrite = false;

    if ( ! (loc & 0x0001)) {
        lc_prewrite = lc_write = false;
    } else {
        if (lc_prewrite) lc_write = true;
        if (!write) lc_prewrite = true;
    }

    lc_bank_one = (loc & 0x0008) != 0;
    lc_read_bsr = ((loc & 0x0002) >> 1) == (loc & 0x0001);

    return 0; // s/b floating bus
}

static byte lc_peek(word loc)
{
    byte val;
    if (rombuf && (!cfg.lang_card || !lc_read_bsr)) {
        val = rombuf[loc - LOC_ROM_START];
    } else if (cfg.lang_card && lc_bank_one && loc < LOC_BSR_END) {
        val = membuf[loc - (LOC_BSR_START - LOC_BSR1_START)];
    } else {
        // Either there's no language card enabled and no ROM,
        // Or the language card is set to bank two, or we're
        //   outside the banked-ram region (0xD000-0xE000)
        val = membuf[loc];
    }
    return val;
}

static bool lc_poke(word loc, byte val)
{
    if (loc < LOC_BSR_START) return false; // write not ours to handle
    if ((rombuf && !cfg.lang_card) || (cfg.lang_card && !lc_write))
        return true; // throw away write

    if (cfg.lang_card && lc_bank_one && loc < LOC_BSR_END) {
        membuf[loc - (LOC_BSR_START - LOC_BSR1_START)] = val;
    } else {
        // Either language card write is enabled for bank two,
        // or language card write is enabled and we're not in $Dxxx,
        // or else there's no language card, and also no ROM
        // (all-ram memory configuration, e.g. for testing).
        membuf[loc] = val;
    }
    return true;
}

byte peek(word loc)
{
    int t = rh_peek(loc);
    if (t < 0) maybe_language_card(loc, false);
    if (t >= 0) {
        return (byte) t;
    }

    return peek_sneaky(loc);
}

byte peek_sneaky(word loc)
{
    int t = -1; // rh_peek_sneaky(loc)?;
    if (t >= 0) {
        return (byte) t;
    }

    if (loc >= LOC_ROM_START) {
        return lc_peek(loc);
    }
    else if ((loc >= cfg.amt_ram && loc < SS_START)
             || (loc >= SS_START && loc < LOC_ROM_START)) {
        return 0; // s/b floating bus? not sure, in sneaky
    }
    else {
        return membuf[loc];
    }
}

void poke(word loc, byte val)
{
    if (rh_poke(loc, val))
        return;
    if (maybe_language_card(loc, true) >= 0)
        return;
    if (lc_poke(loc, val))
        return;
    poke_sneaky(loc, val);
}

void poke_sneaky(word loc, byte val)
{
    // rh_poke_sneaky()?
    // XXX
    membuf[loc] = val;
}

bool mem_match(word loc, unsigned int nargs, ...)
{
    bool status = true;
    va_list args;
    va_start(args, nargs);

    for (; nargs != 0; ++loc, --nargs) {
        if (peek_sneaky(loc) != va_arg(args, int)) {
            status = false;
        }
    }

    va_end(args);
    return status;
}
