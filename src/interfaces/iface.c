#include "bobbin-internal.h"

#include <stdio.h>
#include <unistd.h>

extern IfaceDesc simpleInterface;
#ifdef HAVE_LIBCURSES
extern IfaceDesc ttyInterface;
#endif

static IfaceDesc *iii = NULL;

static struct if_t {
    const char *name;
    IfaceDesc *iface;
} ifs[] = {
#ifdef HAVE_LIBCURSES
    {"tty", &ttyInterface},
#endif
    {"simple", &simpleInterface},
};

static
void load_interface(void)
{
    struct if_t *visit = &ifs[0];
    struct if_t *end = visit + (sizeof ifs)/(sizeof ifs[0]);

    iii = NULL;
    for (; visit != end; ++visit) {
        if (STREQ(visit->name, cfg.interface)) {
            iii = visit->iface;
            return;
        }
    }
}

void interfaces_init(void)
{
    if (cfg.interface == NULL) {
        // No default interface selected?
        // Pick "simple" if stdin isn't a tty;
        // otherwise, pick "tty" (not yet implemented).
        cfg.interface = isatty(0)? "tty" : "simple";
    }

#ifndef HAVE_LIBCURSES
    if (STREQ(cfg.interface, "tty")) {
        DIE(0,"default interface is \"tty\" when stdin is a tty; but that\n");
        DIE(0,"interface has not been included with this build of bobbin\n");
        DIE(0,"(may have been missing a libncurses-dev or similar package?)\n");
        DIE(2,"Try invoking with --simple, instead.\n");
    }
#endif
    load_interface();
    if (iii == NULL) {
        DIE(2,"unsupported interface \"%s\".\n", cfg.interface);
    }
    if (iii->init)
        iii->init();
}

void interfaces_start(void)
{
    if (iii->start)
        iii->start();
}

void iface_prestep(void)
{
    if (iii->prestep)
        iii->prestep();
}

void iface_step(void)
{
    if (iii->step)
        iii->step();
}

int iface_peek(word loc)
{
    return iii->peek? iii->peek(loc) : -1;
}

int iface_poke(word loc, byte val)
{
    return iii->poke? iii->poke(loc, val) : -1;
}

void iface_frame(bool flash)
{
    if (iii->frame)
        iii->frame(flash);
}

void iface_enter_dbg(FILE **inf, FILE **outf)
{
    if (iii->enter_dbg)
        iii->enter_dbg(inf, outf);
}

void iface_exit_dbg(void)
{
    if (iii->exit_dbg)
        iii->exit_dbg();
}
