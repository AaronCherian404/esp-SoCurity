/*
 * collect_esp_monitors.c
 *
 * ESP hardware monitor data collector for OCSVM training data.
 *
 * Samples all ESP performance counters at a configurable interval and writes
 * delta values (counter increments over the window) as CSV rows.  The output
 * format exactly matches the reference bg100ms CSV files used to train the
 * current OCSVM model.
 *
 * CSV column layout (392 columns for CSV_NTILES=9, CSV_NACC=6):
 *   mem stats (13) | acc stats (CSV_NACC×3) | dvfs (CSV_NTILES×4) |
 *   noc_inject (CSV_NTILES×6) | noc_backpressure (CSV_NTILES×6×5) |
 *   Time [| Label]
 *
 * Tiles / accs beyond SOC_NTILES / SOC_NACC emit zero-filled columns so that
 * CSVs from different SoC sizes share the same header (useful when a model
 * trained on a 3×3 SoC is deployed on a 2×2 FPGA prototype).
 *
 * Build:
 *   See Makefile — links against libmonitors from ESP soft/common/drivers.
 *
 * Usage:
 *   collect_esp_monitors -o <output.csv> [OPTIONS]
 *   -o <file>   output CSV file (required)
 *   -n <N>      number of samples; 0 = run until SIGINT (default: 0)
 *   -i <ms>     sampling interval in milliseconds (default: 100)
 *   -a <label>  label string appended as last column (e.g. normal, dos)
 *   -H          suppress CSV header row
 *   -f          flush output after every row (default: flush every 10 rows)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <esp.h>
#include <esp_accelerator.h>
#include "monitors.h"

/* =========================================================================
 * Compile-time CSV format parameters
 *
 * Override at build time with -DCSV_NTILES=N -DCSV_NACC=M to match a
 * specific reference CSV format.  Defaults to the actual SoC size.
 * ========================================================================= */
#ifndef CSV_NTILES
#define CSV_NTILES  SOC_NTILES
#endif

#ifndef CSV_NACC
#define CSV_NACC    SOC_NACC
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */
#define DEFAULT_INTERVAL_MS  100
#define DEFAULT_FLUSH_EVERY  10

/* =========================================================================
 * Globals
 * ========================================================================= */
static volatile sig_atomic_t g_stop = 0;

/* Tile indices derived from soc_locs.h at runtime */
static int g_cpu0_l2_tile = -1;
static int g_mem0_tile    = -1;

/* =========================================================================
 * SIGINT / SIGTERM handler — set flag so the main loop exits cleanly
 * ========================================================================= */
static void handle_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* =========================================================================
 * init_tile_indices
 *
 * Compute g_cpu0_l2_tile and g_mem0_tile from the same soc_locs.h that
 * libmonitors uses.  Including the file inside a function body makes the
 * arrays local — the same idiom used in libmonitors.c::esp_monitor().
 * ========================================================================= */
static void init_tile_indices(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "soc_locs.h"
#pragma GCC diagnostic pop
    (void)acc_locs;
    (void)acc_has_l2;

    g_cpu0_l2_tile = (int)(cpu_locs[0].row * SOC_COLS + cpu_locs[0].col);
    g_mem0_tile    = (int)(mem_locs[0].row * SOC_COLS + mem_locs[0].col);
}

/* =========================================================================
 * write_csv_header
 *
 * Emits the header row that matches the bg100ms reference CSV format.
 * When 'label' is non-NULL a trailing "Label" column is added.
 * ========================================================================= */
static void write_csv_header(FILE *fp, const char *label)
{
    int t, a, p, q, op;

    /* --- 13 memory-tile columns ----------------------------------------- */
    fprintf(fp, "Off-chip memory accesses at mem tile 0");
    fprintf(fp, ",Coherence requests to LLC 0");
    fprintf(fp, ",Coherence forwards from LLC 0");
    fprintf(fp, ",Coherence responses received by LLC 0");
    fprintf(fp, ",Coherence responses sent by LLC 0");
    fprintf(fp, ",DMA requests to mem tile 0");
    fprintf(fp, ",DMA responses from mem tile 0");
    fprintf(fp, ",Coherent DMA requests to LLC 0");
    fprintf(fp, ",Coherent DMA responses from LLC 0");
    fprintf(fp, ",L2 hits for CPU 0");
    fprintf(fp, ",L2 misses for CPU 0");
    fprintf(fp, ",Hits at LLC 0");
    fprintf(fp, ",Misses at LLC 0");

    /* --- CSV_NACC × 3 accelerator columns -------------------------------- */
    for (a = 0; a < CSV_NACC; a++) {
        fprintf(fp, ",Accelerator %d TLB-loading cycles", a);
        fprintf(fp, ",Accelerator %d mem cycles", a);
        fprintf(fp, ",Accelerator %d total cycles", a);
    }

    /* --- CSV_NTILES × DVFS_OP_POINTS DVFS columns ------------------------ */
    for (t = 0; t < CSV_NTILES; t++)
        for (op = 0; op < DVFS_OP_POINTS; op++)
            fprintf(fp, ",DVFS Cycles for tile %d at operating point %d", t, op);

    /* --- CSV_NTILES × NOC_PLANES NoC-inject columns ---------------------- */
    for (t = 0; t < CSV_NTILES; t++)
        for (p = 0; p < NOC_PLANES; p++)
            fprintf(fp, ",NoC packets injected at tile %d on plane %d", t, p);

    /* --- CSV_NTILES × NOC_PLANES × NOC_QUEUES backpressure columns ------- */
    for (t = 0; t < CSV_NTILES; t++)
        for (p = 0; p < NOC_PLANES; p++)
            for (q = 0; q < NOC_QUEUES; q++)
                fprintf(fp, ",NoC backpressure cycles at tile %d on plane %d for queue %d",
                        t, p, q);

    /* --- Time (elapsed µs since previous sample) ------------------------- */
    fprintf(fp, ",Time");

    /* --- Optional label column ------------------------------------------ */
    if (label)
        fprintf(fp, ",Label");

    fprintf(fp, "\n");
}

/* =========================================================================
 * write_csv_row
 *
 * Emits one data row corresponding to the counter deltas in *d*.
 * elapsed_us is the wall-clock time of the sampling window in microseconds.
 * ========================================================================= */
static void write_csv_row(FILE *fp, const esp_monitor_vals_t *d,
                          uint64_t elapsed_us, const char *label)
{
    int t, a, p, q, op;

    /* --- 13 memory-tile columns ----------------------------------------- */
    fprintf(fp, "%u",  d->ddr_accesses[0]);
    fprintf(fp, ",%u", d->mem_reqs[0].coh_reqs);
    fprintf(fp, ",%u", d->mem_reqs[0].coh_fwds);
    fprintf(fp, ",%u", d->mem_reqs[0].coh_rsps_rcv);
    fprintf(fp, ",%u", d->mem_reqs[0].coh_rsps_snd);
    fprintf(fp, ",%u", d->mem_reqs[0].dma_reqs);
    fprintf(fp, ",%u", d->mem_reqs[0].dma_rsps);
    fprintf(fp, ",%u", d->mem_reqs[0].coh_dma_reqs);
    fprintf(fp, ",%u", d->mem_reqs[0].coh_dma_rsps);

    /* L2 hits/misses for CPU 0 */
    if (g_cpu0_l2_tile >= 0 && g_cpu0_l2_tile < SOC_NTILES) {
        fprintf(fp, ",%u,%u",
                d->l2_stats[g_cpu0_l2_tile].hits,
                d->l2_stats[g_cpu0_l2_tile].misses);
    } else {
        fprintf(fp, ",0,0");
    }

    /* LLC hits/misses */
    fprintf(fp, ",%u,%u", d->llc_stats[0].hits, d->llc_stats[0].misses);

    /* --- CSV_NACC × 3 accelerator columns -------------------------------- */
    for (a = 0; a < CSV_NACC; a++) {
        if (a < SOC_NACC) {
            uint64_t acc_mem = (uint64_t)d->acc_stats[a].acc_mem_lo
                             | ((uint64_t)d->acc_stats[a].acc_mem_hi << 32);
            uint64_t acc_tot = (uint64_t)d->acc_stats[a].acc_tot_lo
                             | ((uint64_t)d->acc_stats[a].acc_tot_hi << 32);
            fprintf(fp, ",%u,%llu,%llu",
                    d->acc_stats[a].acc_tlb,
                    (unsigned long long)acc_mem,
                    (unsigned long long)acc_tot);
        } else {
            fprintf(fp, ",0,0,0");
        }
    }

    /* --- CSV_NTILES × DVFS_OP_POINTS DVFS columns ------------------------ */
    for (t = 0; t < CSV_NTILES; t++) {
        for (op = 0; op < DVFS_OP_POINTS; op++) {
            if (t < SOC_NTILES)
                fprintf(fp, ",%u", d->dvfs_op[t][op]);
            else
                fprintf(fp, ",0");
        }
    }

    /* --- CSV_NTILES × NOC_PLANES NoC-inject columns ---------------------- */
    for (t = 0; t < CSV_NTILES; t++) {
        for (p = 0; p < NOC_PLANES; p++) {
            if (t < SOC_NTILES)
                fprintf(fp, ",%u", d->noc_injects[t][p]);
            else
                fprintf(fp, ",0");
        }
    }

    /* --- CSV_NTILES × NOC_PLANES × NOC_QUEUES backpressure columns ------- */
    for (t = 0; t < CSV_NTILES; t++) {
        for (p = 0; p < NOC_PLANES; p++) {
            for (q = 0; q < NOC_QUEUES; q++) {
                if (t < SOC_NTILES)
                    fprintf(fp, ",%u", d->noc_queue_full[t][p][q]);
                else
                    fprintf(fp, ",0");
            }
        }
    }

    /* --- Time (elapsed µs) ----------------------------------------------- */
    fprintf(fp, ",%llu", (unsigned long long)elapsed_us);

    /* --- Optional label -------------------------------------------------- */
    if (label)
        fprintf(fp, ",%s", label);

    fprintf(fp, "\n");
}

/* =========================================================================
 * print_usage
 * ========================================================================= */
static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -o <output.csv> [OPTIONS]\n", prog);
    fprintf(stderr, "  -o <file>  output CSV file (required)\n");
    fprintf(stderr, "  -n <N>     number of samples; 0 = run until SIGINT (default: 0)\n");
    fprintf(stderr, "  -i <ms>    sampling interval in milliseconds (default: %d)\n",
            DEFAULT_INTERVAL_MS);
    fprintf(stderr, "  -a <label> label column appended to each row (e.g. normal, dos)\n");
    fprintf(stderr, "  -H         suppress CSV header row\n");
    fprintf(stderr, "  -f         flush after every row instead of every %d rows\n",
            DEFAULT_FLUSH_EVERY);
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char *argv[])
{
    const char  *outfile      = NULL;
    const char  *label        = NULL;
    long         num_samples  = 0;          /* 0 = unlimited */
    long         interval_ms  = DEFAULT_INTERVAL_MS;
    int          no_header    = 0;
    int          flush_every  = 0;          /* 1 = flush every row */
    int          opt;

    struct sigaction    sa;
    esp_monitor_args_t  args;
    esp_monitor_vals_t  v0, v1, diff;
    struct timeval      tv0, tv1;
    FILE               *fp;
    long                count       = 0;
    int                 flush_count = 0;

    /* ---- argument parsing -------------------------------------------- */
    while ((opt = getopt(argc, argv, "o:n:i:a:Hfh")) != -1) {
        switch (opt) {
        case 'o': outfile     = optarg;       break;
        case 'n': num_samples = atol(optarg); break;
        case 'i': interval_ms = atol(optarg); break;
        case 'a': label       = optarg;       break;
        case 'H': no_header   = 1;            break;
        case 'f': flush_every = 1;            break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (!outfile) {
        fprintf(stderr, "Error: -o <output.csv> is required.\n");
        print_usage(argv[0]);
        return 1;
    }
    if (interval_ms <= 0) {
        fprintf(stderr, "Error: interval must be > 0 ms.\n");
        return 1;
    }

    /* ---- optional CPU affinity ---------------------------------------- */
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
            fprintf(stderr, "Warning: could not pin to CPU 0: %s\n",
                    strerror(errno));
    }

    /* ---- tile index initialisation ------------------------------------ */
    init_tile_indices();

    /* ---- open output file --------------------------------------------- */
    fp = fopen(outfile, "w");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", outfile, strerror(errno));
        return 1;
    }

    /* ---- install signal handlers -------------------------------------- */
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* ---- startup banner ----------------------------------------------- */
    fprintf(stderr, "ESP monitor data collector\n");
    fprintf(stderr, "  Output   : %s\n", outfile);
    fprintf(stderr, "  Interval : %ld ms\n", interval_ms);
    fprintf(stderr, "  Samples  : %s\n",
            num_samples == 0 ? "unlimited (SIGINT to stop)" : "");
    fprintf(stderr, "  SoC      : %d (%d tiles, %d mem, %d acc)\n",
            SOC_ROWS * SOC_COLS, SOC_NTILES, SOC_NMEM, SOC_NACC);
    fprintf(stderr, "  CSV cols : %d tiles, %d acc\n", CSV_NTILES, CSV_NACC);
    fprintf(stderr, "  CPU0 L2  : tile %d\n", g_cpu0_l2_tile);
    fprintf(stderr, "  Mem0 LLC : tile %d\n", g_mem0_tile);
    fprintf(stderr, "  Label    : %s\n", label ? label : "(none)");

    /* ---- optional CSV header ------------------------------------------ */
    if (!no_header)
        write_csv_header(fp, label);

    /* ---- initialise monitor args -------------------------------------- */
    memset(&args, 0, sizeof(args));
    args.read_mode = ESP_MON_READ_ALL;

    /* ---- baseline read ------------------------------------------------ */
    esp_monitor(args, &v0);
    gettimeofday(&tv0, NULL);

    /* ---- sampling loop ------------------------------------------------ */
    while (!g_stop && (num_samples == 0 || count < num_samples)) {

        usleep((unsigned int)(interval_ms * 1000));

        esp_monitor(args, &v1);
        gettimeofday(&tv1, NULL);

        diff = esp_monitor_diff(v0, v1);

        uint64_t elapsed_us = (uint64_t)(tv1.tv_sec  - tv0.tv_sec)  * 1000000ULL
                            + (uint64_t)(tv1.tv_usec - tv0.tv_usec);

        write_csv_row(fp, &diff, elapsed_us, label);
        count++;

        /* roll baseline forward */
        v0  = v1;
        tv0 = tv1;

        /* flush logic */
        if (flush_every) {
            fflush(fp);
        } else {
            if (++flush_count >= DEFAULT_FLUSH_EVERY) {
                fflush(fp);
                flush_count = 0;
            }
        }
    }

    fflush(fp);
    fclose(fp);

    fprintf(stderr, "  collected %ld samples (%.1f s)\n",
            count, (double)count * interval_ms / 1000.0);
    fprintf(stderr, "Done. %ld samples written to %s\n", count, outfile);

    return 0;
}
