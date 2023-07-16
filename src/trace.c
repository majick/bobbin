#include "bobbin-internal.h"
#include "iface-simple.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

word current_instruction = 0;

FILE *trfile;

static int traceon = 0;

static void do_rts(void)
{
    byte lo = stack_pop();
    byte hi = stack_pop();
    go_to(WORD(lo, hi)+1);
}

static void bobbin_hooks(void)
{
#define BTRACE 1
    if (BTRACE && !traceon)
        trace_on("6502 TESTS");

    switch (current_instruction) {
        case 0:
            // report_init
            do_rts();
            break;
        case 1:
            fputs("*** REPORT ERROR ***\n", stderr);
            fprintf(stderr, "Failed testcase: %02X\n",
                    mem_get_byte_nobus(0x200));
            do_rts(); current_instruction = PC;
            util_print_state(stderr);
            exit(3);
            break;
        case 2:
            fputs(".-= !!! REPORT SUCCESS !!! =-.\n", stderr);
            exit(0);
            break;
        default:
            ;
    }
}

void trace_instr(void)
{
    current_instruction = PC;

    if (traceon)
        util_print_state(trfile);

    if (bobbin_test)
        bobbin_hooks();

    iface_simple_instr_hook();
}

int trace_mem_get_byte(word loc)
{
    return iface_simple_getb_hook(loc);
}

void trace_on(char *format, ...)
{
    va_list args;

    fprintf(trfile, "\n\n~~~ TRACING STARTED: ");
    va_start(args, format);
    vfprintf(trfile, format, args);
    va_end(args);
    fprintf(trfile, " ~~~\n");
    traceon = 1;
}

void trace_off(void)
{
    fprintf(trfile, "~~~ TRACING FINISHED ~~~\n");
    traceon = 0;
}

int tracing(void)
{
    return traceon;
}
