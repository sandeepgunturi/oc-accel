/*
 * Copyright 2020 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <endian.h>
#include <asm/byteorder.h>
#include <sys/mman.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <libocaccel.h>
#include "force_cpu.h"

static int quiet = 0;
static const char* version = GIT_VERSION;

/**
 * @brief Prints valid command line options
 *
 * @param prog        current program name
 */
static void usage (const char* prog)
{
    printf ("Usage: %s [-h] \n"
            "  -C,--card <cardno> can be (0...3)\n"
            "  -V, --version             print version.\n"
            "  -q, --quiet               quiece output.\n"
            "  -w, --width <32|64>       access width, 64: default\n"
            "  -X, --cpu <id>            only run on this CPU.\n"
            "  -i, --interval <intv>     interval in usec, 0: default.\n"
            "  -c, --count <mum>         number of pokes, 1: default\n"
            "  -r, --read-back           read back and verify.\n"
            "  <addr> <val>\n"
            "\n"
            "Example:\n"
            "  ocaccel_poke 0x0000000 0xdeadbeef\n"
            "\n",
            prog);
}

/**
 * Write to OCACCEL specific registers. Must be called as root!
 */
int main (int argc, char* argv[])
{
    int ch, rc, rbrc = 0;
    int card_no = 0;
    struct ocaccel_card* card;
    int cpu = -1;
    int width = 64;
    int rd_back = 0;
    uint32_t offs;
    uint64_t val, rbval;
    unsigned long i, count = 1;
    unsigned long interval = 0;
    int xerrno;
    char device[128];
    int verbose_flag = ocaccel_action_trace_enabled();

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
            /* options */
            { "card"     , required_argument , NULL , 'C' } ,
            { "cpu"      , required_argument , NULL , 'X' } ,
            { "width"    , required_argument , NULL , 'w' } ,
            { "interval" , required_argument , NULL , 'i' } ,
            { "count"    , required_argument , NULL , 'c' } ,
            { "rd-back"  , no_argument       , NULL , 'r' } ,
            { "version"  , no_argument       , NULL , 'V' } ,
            { "quiet"    , no_argument       , NULL , 'q' } ,
            { "help"     , no_argument       , NULL , 'h' } ,
            { 0          , no_argument       , NULL , 0   } ,
        };

        ch = getopt_long (argc, argv, "p:C:X:w:i:c:Vqrvh",
                          long_options, &option_index);

        if (ch == -1) {      /* all params processed ? */
            break;
        }

        switch (ch) {
        /* which card to use */
        case 'C':
            card_no = strtol (optarg, (char**)NULL, 0);
            break;

        case 'X':
            cpu = strtoul (optarg, NULL, 0);
            break;

        case 'w':
            width = strtoul (optarg, NULL, 0);
            break;

        case 'i':                /* interval */
            interval = strtol (optarg, (char**)NULL, 0);
            break;

        case 'c':                /* loop count */
            count = strtol (optarg, (char**)NULL, 0);
            break;

        case 'V':
            printf ("%s\n", version);
            exit (EXIT_SUCCESS);

        case 'q':
            quiet++;
            break;

        case 'r':
            rd_back++;
            break;

        case 'h':
            usage (argv[0]);
            exit (EXIT_SUCCESS);
            break;

        default:
            usage (argv[0]);
            exit (EXIT_FAILURE);
        }
    }

    if (optind + 2 != argc) {
        usage (argv[0]);
        exit (EXIT_FAILURE);
    }

    offs = strtoull (argv[optind++], NULL, 0);
    val  = strtoull (argv[optind++], NULL, 0);
    rbval = ~val;
    switch_cpu (cpu, verbose_flag);

    if ((card_no < 0) || (card_no > 4)) {
        fprintf (stderr, "err: (%d) is a invalid card number!\n",
                 card_no);
        usage (argv[0]);
        exit (EXIT_FAILURE);
    }

    if (card_no == 0) {
        snprintf (device, sizeof (device) - 1, "IBM,oc-accel");
    } else {
        snprintf (device, sizeof (device) - 1, "/dev/ocxl/IBM,oc-accel.000%d:00:00.1.0", card_no);
    }

    card = ocaccel_card_alloc_dev (device, OCACCEL_VENDOR_ID_ANY,
                                   OCACCEL_DEVICE_ID_ANY);

    if (card == NULL) {
        fprintf (stderr, "err: failed to open card %u: %s\n", card_no,
                 strerror (errno));
        exit (EXIT_FAILURE);
    }

    for (i = 0; i < count; i++) {
        switch (width) {
        case 32:
            rc = ocaccel_action_write32 (card, offs, (uint32_t)val);
            xerrno = errno;

            if (rd_back)
                rbrc = ocaccel_action_read32 (card, offs,
                                              (uint32_t*)&rbval);

            break;

        default:
        case 64:
            printf ("Poke OCACCEL Global regs: 0x%x, 0x%lx\n", offs, val);
            rc = ocaccel_global_write64 (card, offs, val);
            xerrno = errno;

            if (rd_back) {
                rbrc = ocaccel_global_read64 (card, offs, &rbval);
            }

            break;
        }

        if (rc != 0) {
            fprintf (stderr, "err: could not write "
                     "%016llx to [%08x]\n"
                     "  %s\n", (unsigned long long)val, offs,
                     strerror (xerrno));
            ocaccel_card_free (card);
            exit (EXIT_FAILURE);
        }

        if (rd_back) {
            if (rbrc != 0) {
                fprintf (stderr, "err: read back failed (%d)\n",
                         rbrc);
                ocaccel_card_free (card);
                exit (EXIT_FAILURE);
            }

            if (val != rbval) {
                fprintf (stderr, "err: post verify failed "
                         "%016llx/%016llx\n",
                         (unsigned long long)val,
                         (unsigned long long)rbval);
                ocaccel_card_free (card);
                exit (EXIT_FAILURE);
            }
        }

        if (interval) {
            usleep (interval);
        }
    }

    ocaccel_card_free (card);

    if (!quiet) {
        printf ("[%08x] %016llx\n", offs, (long long)val);
    }

    exit (EXIT_SUCCESS);
}
