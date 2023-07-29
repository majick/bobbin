#include "bobbin-internal.h"

/* For the ROM file, and error handling */
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Enough of a RAM buffer to provide 128k
//  Note: memory at 0xC000 thru 0xCFFF, and 0x1C000 thru 0x1CFFF,
//  are read at alternate banks of RAM for locations $D000 - $DFFF
static byte membuf[128 * 1024];

// Pointer to firmware, mapped into the Apple starting at $D000
static unsigned char *rombuf;

static const char * const rom_dirs[] = {
    "BOBBIN_ROMDIR", // not a dirname, an env var name
    "./roms",        // also not a dirname; we'll use bobbin's dir instead
    ROMSRCHDIR "/roms",
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

    rombuf = mmap(NULL, 12 * 1024, PROT_READ, MAP_PRIVATE, fd, 0);
    if (rombuf == NULL) {
        perror("Couldn't map in ROM file");
        exit(1);
    }
    close(fd); // safe to close now.

    validate_rom(rombuf, 12 * 1024);
}

static void load_ram(void)
{
    errno = 0;
    FILE *file = fopen(cfg.ram_load_file, "rb");
    if (file == NULL) {
        DIE(1, "Couldn't open --load file \"%s\": %s\n",
            cfg.ram_load_file, strerror(errno));
    }

    errno = 0;
    size_t rb = fread(&membuf[cfg.ram_load_loc], 1,
                      (sizeof membuf) - cfg.ram_load_loc, file);
    int err = errno;
    if (ferror(file)) {
        DIE(1, "Error reading from --load file \"%s\": %s\n",
            cfg.ram_load_file, strerror(err));
    }

    INFO("%zu bytes read into RAM from file \"%s\",\n",
         (size_t)rb, cfg.ram_load_file);
    INFO("  starting at memory location $%04X.\n",
         (unsigned int)cfg.ram_load_loc);
    // Warning, this ^ is a lie for some values ($C000 - $CFFF), and
    //  aux mem locations

    fclose(file);
}

void mem_init(void)
{
    if (cfg.ram_load_loc >= sizeof membuf) {
        DIE(2, "--load-at value must be less than $20000 (128k).\n");
    }

    /* Immitate the on-boot memory pattern. */
    for (size_t z=0; z != sizeof membuf; ++z) {
        if (!(z & 0x2))
            membuf[z] = 0xFF;
    }

    if (cfg.load_rom) {
        load_rom();
    } else if (cfg.ram_load_file == NULL) {
        DIE(2, "--no-rom given, but not --load!\n");
    }

    if (cfg.ram_load_file != NULL) {
        load_ram();
    }
}

byte peek(word loc)
{
    int t = rh_peek(loc);
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

    if (rombuf && loc >= 0xD000) {
        return rombuf[loc - 0xD000];
    }
    else if ((loc >= cfg.amt_ram && loc < 0xC000)
             || (loc >= 0xC000 && loc < 0xD000)) {
        return 0;
    }
    else {
        return membuf[loc];
    }
}

void poke(word loc, byte val)
{
    if (!rh_poke(loc, val))
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
