/*
 * background_monitor.c
 *
 * Enhanced ESP background monitor with integrated OCSVM anomaly detection.
 *
 * Runs on Linux (Ariane core) via the ESP Monitors API. Continuously samples
 * all ESP hardware performance counters, extracts the same 6-feature vector
 * used by the bare-metal Ibex detector, runs OCSVM inference, and writes:
 *   - Raw monitor deltas (all counters via esp_monitor_print)
 *   - Security-relevant counter summary (NoC, DDR, DMA, coherence)
 *   - OCSVM detection result and decision score
 *   - Running anomaly statistics
 *
 * Build:
 *   See Makefile — links against libmonitors from ESP soft/common/drivers.
 *
 * Usage:
 *   ./background_monitor.exe -o <output_file>
 *
 * Output file format per sampling interval:
 *   --- Sample N (elapsed usec) ---
 *   [RAW MONITORS]  <esp_monitor_print output>
 *   [SECURITY FEATURES]  ddr, coh, dma, noc, acc fields
 *   [OCSVM]  score=<val>  label=<regular|ANOMALY>
 *   [STATS]  total=N  anomalies=M  rate=P%
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sched.h>

#include <esp.h>
#include <esp_accelerator.h>
#include "monitors.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

#define USLEEP_USECS   100000   /* 100 ms sampling interval */
#define FEATURE_COUNT  6
#define NOC_PLANES     6        /* ESP has 6 NoC planes */
#define SEC_TILE       4        /* Tile used for NoC injection feature */
#define SEC_PLANE      4        /* NoC plane used for injection feature */

#define SOCURITY_ALERT_BASE_DEFAULT 0x60010000UL
#define SOCURITY_ALERT_TRIGGER      0x00UL
#define SOCURITY_ALERT_ITERATION    0x04UL
#define SOCURITY_ALERT_CLEAR        0x08UL
#define SOCURITY_ALERT_STATUS       0x0CUL

/* =========================================================================
 * OCSVM model parameters (same as ocsvm_detector.c / ocsvm_model.h)
 * Double precision — FPU available on Linux/Ariane.
 *
 * score > 0  =>  NORMAL
 * score <= 0 =>  ANOMALY
 * ========================================================================= */

#define OCSVM_N_SVS  33
#define OCSVM_GAMMA  2.723389230434805
#define OCSVM_RHO    2.5954405797397517

static const double sv[OCSVM_N_SVS][FEATURE_COUNT] = {
    { 0.998971424382511,    0.35355879303920523, 0.0,                  0.0,                  0.0,                  0.0                  },
    { 0.99355823020851,     0.36292568831370065, 0.0,                  0.4363833243096914,   0.0,                  0.0                  },
    { 0.8186408140637671,   1.0,                 0.0,                  0.8597726042230643,   0.0,                  0.0                  },
    { 0.23857914534300362,  0.22399386116040298, 0.9985059760956175,   0.3448835950189496,   0.49973103819257664,  0.6216893971305893   },
    { 0.243941941602562,    0.22145937630275103, 0.5809262948207171,   0.23335138061721705,  0.0,                  0.9999967782960247   },
    { 0.33714372110710467,  0.17133781694895245, 0.8381474103585658,   0.43963183540877093,  0.9999327595481441,   0.02899036898444105  },
    { 0.4124977664560644,   0.32776386127107926, 0.8713894422310757,   0.76285868976719,     0.0,                  0.36455902792599854  },
    { 0.391360422975951,    0.2805383290108131,  0.9992529880478087,   0.40443963183540876,  0.49991594943518014,  0.05873636178876522  },
    { 0.20086088801125246,  0.13583289370289342, 0.5809262948207171,   0.7487818083378451,   0.0,                  0.8540568099170492   },
    { 0.2619271246157159,   0.12570971109823992, 0.5476842629482072,   0.4147265836491608,   0.9995041016675631,   0.014494848898056427 },
    { 0.5274141746425184,   0.9202356665104902,  0.0,                  1.0,                  0.0,                  0.0                  },
    { 0.2871604440514425,   0.2785682927459133,  0.0,                  0.7060097455332973,   0.0,                  0.0                  },
    { 0.8100113166226066,   0.7835690121411786,  0.0,                  0.9225771521386031,   0.0,                  0.0                  },
    { 0.8590507781896153,   0.9832915838132376,  0.0,                  0.41256090958310776,  0.0,                  0.0                  },
    { 0.42371359323385094,  0.30376188385640135, 0.8713894422310757,   0.7720628045479155,   0.0,                  0.5435795869614418   },
    { 0.24512171096337898,  0.2125351858068848,  0.5809262948207171,   0.23335138061721705,  0.0,                  0.9999970467713559   },
    { 0.005236801473451735, 0.02264066022039324, 0.29046314741035856,  0.6307525717379535,   0.0,                  0.49999906033634056  },
    { 0.35886751304618736,  0.18748916295593987, 0.5476842629482072,   0.936654033567948,    0.9998403039268423,   0.014495520086384623 },
    { 0.0,                  0.01334385986918074, 0.29046314741035856,  0.19599350297780183,  0.0,                  0.4999993288116718   },
    { 0.5153438741334995,   0.44057610648525614, 0.7095368525896414,   0.9247428262046563,   0.49975625336202256,  0.028989160845450297 },
    { 0.35084737220692463,  0.241550794839538,   0.5476842629482072,   0.9409853817000541,   0.9997310381925766,   0.014492566857740561 },
    { 0.06314629598236987,  0.012052637598179039,0.41832669322709165,  0.30319436924742826,  0.49995797471759007,  0.455355237161711    },
    { 0.23101029491943204,  0.1776943197287696,  0.0,                  0.2079047103410936,   0.0,                  0.0                  },
    { 1.0,                  0.351714189794917,   0.0,                  0.0,                  0.0,                  0.0                  },
    { 0.8584528756591818,   0.9801077986135963,  0.0,                  0.4093123984840281,   0.0,                  0.0                  },
    { 0.6428941231450132,   0.4824338433046437,  0.1286105577689243,   0.3259339469409853,   0.4997478483055406,   0.0                  },
    { 0.448772810783321,    0.39545342192347854, 0.5816733067729084,   0.7682728749323228,   0.0,                  0.09507476635263107  },
    { 0.06898787242913362,  0.056555535469875795,0.4190737051792829,   0.7579859231185707,   0.49984870898332434,  0.49999919457400616  },
    { 0.34166349770689486,  0.17275816144705436, 0.8381474103585658,   0.43963183540877093,  0.9998907342657342,   0.028992516787091277 },
    { 0.3775009965042174,   0.2413183748307577,  0.8381474103585658,   0.9144558743909043,   0.999537721893491,    0.02899197983642872  },
    { 0.4206026674241625,   0.3234991385702849,  0.9992529880478087,   0.860855441256091,    0.49990754437869817,  0.2937136232705826   },
    { 0.3189454923647159,   0.23827109027119364, 0.00149402390438247,  0.28965890633459657,  0.0,                  0.5943937786748433   },
    { 0.505610204203186,    0.4733952874076315,  0.8713894422310757,   0.3627504060638873,   0.0,                  0.36455862521300164  },
};

static const double alpha[OCSVM_N_SVS] = {
    0.7155572628020521,   0.24450224899437176,  0.863793892598006,
    0.9273229371813887,   1.0,                  0.09682885443219426,
    0.025085214128138484, 0.49750911296508443,  0.7341380440953482,
    0.9916259126264291,   1.0,                  0.8053572378812665,
    0.06163946689233884,  1.0,                  0.49793849082137576,
    0.24852652895264848,  0.1599345788669015,   0.3974653111554286,
    0.7258076149340595,   0.5782930025757486,   0.42736354363718915,
    0.16218087063885514,  0.8901775803732325,   1.0,
    0.016723509172182398, 0.6535774320371934,   0.058548878949206455,
    0.6671190519318897,   0.2493470738790698,   0.6949602205028103,
    0.2932154871254299,   0.5929764095382377,   0.8724842303112711,
};

/* MinMaxScaler parameters (matching ocsvm_detector.c) */
static const double scaler_min[FEATURE_COUNT] = {
    -0.43979519, -0.42732443, 0.0, -0.56083758, 0.0, 0.0
};
static const double scaler_scale[FEATURE_COUNT] = {
    2.29605308e-6, 3.74153478e-6, 1.24501992e-4,
    5.65930956e-4, 8.40533907e-6, 1.34237521e-7
};

/* =========================================================================
 * OCSVM inference (double precision, FPU available on Linux)
 * ========================================================================= */

static double rbf_kernel_d(const double *x, const double *sv_i)
{
    double dist_sq = 0.0;
    for (int i = 0; i < FEATURE_COUNT; i++) {
        double d = x[i] - sv_i[i];
        dist_sq += d * d;
    }
    return exp(-OCSVM_GAMMA * dist_sq);
}

static double ocsvm_score_d(const double *x_scaled)
{
    double score = 0.0;
    for (int i = 0; i < OCSVM_N_SVS; i++)
        score += alpha[i] * rbf_kernel_d(x_scaled, sv[i]);
    return score - OCSVM_RHO;
}

static void min_max_scale_d(const double *raw, double *scaled)
{
    for (int i = 0; i < FEATURE_COUNT; i++)
        scaled[i] = raw[i] * scaler_scale[i] + scaler_min[i];
}

/* =========================================================================
 * Feature extraction — mirrors ocsvm_detector.c get_features()
 * ========================================================================= */

static void get_features(const esp_monitor_vals_t *vals, double *feat)
{
    feat[0] = (double)vals->ddr_accesses[0];
    feat[1] = (double)vals->mem_reqs[0].coh_rsps_snd;
    feat[2] = (double)vals->mem_reqs[0].dma_reqs;
    feat[3] = (double)vals->noc_injects[SEC_TILE][SEC_PLANE];

    feat[4]  = (double)((uint64_t)vals->acc_stats[0].acc_tot_lo
                       | ((uint64_t)vals->acc_stats[0].acc_tot_hi << 32));
    feat[4] += (double)((uint64_t)vals->acc_stats[1].acc_tot_lo
                       | ((uint64_t)vals->acc_stats[1].acc_tot_hi << 32));
    feat[4] += (double)((uint64_t)vals->acc_stats[2].acc_tot_lo
                       | ((uint64_t)vals->acc_stats[2].acc_tot_hi << 32));

    feat[5]  = (double)((uint64_t)vals->acc_stats[3].acc_tot_lo
                       | ((uint64_t)vals->acc_stats[3].acc_tot_hi << 32));
    feat[5] += (double)((uint64_t)vals->acc_stats[4].acc_tot_lo
                       | ((uint64_t)vals->acc_stats[4].acc_tot_hi << 32));
    feat[5] += (double)((uint64_t)vals->acc_stats[5].acc_tot_lo
                       | ((uint64_t)vals->acc_stats[5].acc_tot_hi << 32));
}

/* =========================================================================
 * Print security-relevant counters to output file
 * ========================================================================= */

static void print_security_features(FILE *fp, const esp_monitor_vals_t *vals)
{
    fprintf(fp, "[SECURITY FEATURES]\n");
    fprintf(fp, "  f0_ddr_accesses      = %u\n",  vals->ddr_accesses[0]);
    fprintf(fp, "  f1_coh_rsps_snd      = %u\n",  vals->mem_reqs[0].coh_rsps_snd);
    fprintf(fp, "  f2_dma_reqs          = %u\n",  vals->mem_reqs[0].dma_reqs);
    fprintf(fp, "  f3_noc_inj[t%d][p%d] = %u\n",
            SEC_TILE, SEC_PLANE, vals->noc_injects[SEC_TILE][SEC_PLANE]);

    uint64_t acc012 =
        ((uint64_t)vals->acc_stats[0].acc_tot_lo | ((uint64_t)vals->acc_stats[0].acc_tot_hi << 32)) +
        ((uint64_t)vals->acc_stats[1].acc_tot_lo | ((uint64_t)vals->acc_stats[1].acc_tot_hi << 32)) +
        ((uint64_t)vals->acc_stats[2].acc_tot_lo | ((uint64_t)vals->acc_stats[2].acc_tot_hi << 32));
    uint64_t acc345 =
        ((uint64_t)vals->acc_stats[3].acc_tot_lo | ((uint64_t)vals->acc_stats[3].acc_tot_hi << 32)) +
        ((uint64_t)vals->acc_stats[4].acc_tot_lo | ((uint64_t)vals->acc_stats[4].acc_tot_hi << 32)) +
        ((uint64_t)vals->acc_stats[5].acc_tot_lo | ((uint64_t)vals->acc_stats[5].acc_tot_hi << 32));
    fprintf(fp, "  f4_acc_tot_012       = %llu\n", (unsigned long long)acc012);
    fprintf(fp, "  f5_acc_tot_345       = %llu\n", (unsigned long long)acc345);

    /* All 6 NoC planes for the security tile */
    fprintf(fp, "  noc_inject_tile%d    =", SEC_TILE);
    for (int p = 0; p < NOC_PLANES; p++)
        fprintf(fp, " p%d:%u", p, vals->noc_injects[SEC_TILE][p]);
    fprintf(fp, "\n");

    /* Additional coherence fields */
    fprintf(fp, "  coh_rsps_rcv         = %u\n", vals->mem_reqs[0].coh_rsps_rcv);
    fprintf(fp, "  coh_rsps_snd_extra   = %u\n", vals->mem_reqs[0].coh_rsps_snd);
    fprintf(fp, "  dma_rsps             = %u\n", vals->mem_reqs[0].dma_rsps);
}

/* =========================================================================
 * Usage
 * ========================================================================= */

static void print_usage(const char *pname)
{
    fprintf(stderr, "Usage: %s <OPTIONS>\n", pname);
    fprintf(stderr, "  -h          print this help\n");
    fprintf(stderr, "  -o <file>   write monitor + detection output to <file>\n");
}

typedef struct {
    int fd;
    void *map_base;
    size_t map_len;
    volatile uint32_t *regs;
    uint32_t phys_base;
} socurity_irq_ctx_t;

static uint32_t parse_u32_env_hex(const char *name, uint32_t default_val)
{
    const char *value = getenv(name);
    char *endptr;
    unsigned long parsed;

    if (!value || !*value)
        return default_val;

    parsed = strtoul(value, &endptr, 0);
    if (*endptr != '\0' || parsed > 0xFFFFFFFFUL)
        return default_val;

    return (uint32_t)parsed;
}

static int socurity_irq_init(socurity_irq_ctx_t *ctx)
{
    long page_size = sysconf(_SC_PAGESIZE);
    uint32_t aligned;
    uint32_t page_off;

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->phys_base = parse_u32_env_hex("SOCURITY_ALERT_BASE", SOCURITY_ALERT_BASE_DEFAULT);

    if (page_size <= 0)
        page_size = 4096;

    aligned = ctx->phys_base & ~((uint32_t)page_size - 1U);
    page_off = ctx->phys_base - aligned;

    ctx->fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (ctx->fd < 0)
        return -1;

    ctx->map_len = (size_t)page_size;
    ctx->map_base = mmap(NULL, ctx->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, (off_t)aligned);
    if (ctx->map_base == MAP_FAILED) {
        ctx->map_base = NULL;
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    ctx->regs = (volatile uint32_t *)((volatile uint8_t *)ctx->map_base + page_off);
    return 0;
}

static void socurity_irq_close(socurity_irq_ctx_t *ctx)
{
    if (ctx->map_base)
        munmap(ctx->map_base, ctx->map_len);
    if (ctx->fd >= 0)
        close(ctx->fd);
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
}

static inline uint32_t socurity_irq_read(const socurity_irq_ctx_t *ctx, uint32_t reg_off)
{
    return ctx->regs[reg_off >> 2];
}

static inline void socurity_irq_write(const socurity_irq_ctx_t *ctx, uint32_t reg_off, uint32_t value)
{
    ctx->regs[reg_off >> 2] = value;
}

static int socurity_irq_poll_and_clear(const socurity_irq_ctx_t *ctx, uint32_t *iteration)
{
    uint32_t status;

    if (!ctx->regs)
        return 0;

    status = socurity_irq_read(ctx, SOCURITY_ALERT_STATUS);
    if ((status & 1U) == 0)
        return 0;

    *iteration = socurity_irq_read(ctx, SOCURITY_ALERT_ITERATION);
    socurity_irq_write(ctx, SOCURITY_ALERT_CLEAR, 1U);
    return 1;
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    /* Pin to core 0 for consistent timing */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
        fprintf(stderr, "Warning: could not set thread affinity\n");

    /* Parse arguments */
    int  opt;
    char esp_mon_file[256] = {0};
    int  has_output_file   = 0;

    while ((opt = getopt(argc, argv, ":ho:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'o':
                snprintf(esp_mon_file, sizeof(esp_mon_file) - 1, "%s", optarg);
                has_output_file = 1;
                break;
            case ':':
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                print_usage(argv[0]);
                return 1;
            default:
                break;
        }
    }

    if (!has_output_file) {
        fprintf(stderr, "Error: no output file specified. Use -o <file>.\n");
        print_usage(argv[0]);
        return 1;
    }

    FILE *fp = fopen(esp_mon_file, "w");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    /* Monitor setup */
    esp_monitor_vals_t vals_start, vals_curr, vals_diff;
    esp_monitor_args_t mon_args;
    mon_args.read_mode = ESP_MON_READ_ALL;

    struct timeval tv_prev, tv_curr;
    uint64_t total_samples   = 0;
    uint64_t total_anomalies = 0;
    uint64_t total_irq_events = 0;
    socurity_irq_ctx_t irq_ctx;
    int irq_mapped = 0;

    if (socurity_irq_init(&irq_ctx) == 0) {
        irq_mapped = 1;
    } else {
        irq_ctx.fd = -1;
        irq_ctx.regs = NULL;
    }

    printf("SoCurity background monitor starting. Output: %s\n", esp_mon_file);
    fprintf(fp, "# SoCurity ESP Background Monitor\n");
    fprintf(fp, "# Sampling interval: %d us\n", USLEEP_USECS);
    fprintf(fp, "# OCSVM: %d SVs, gamma=%.4f, rho=%.4f\n\n",
            OCSVM_N_SVS, OCSVM_GAMMA, OCSVM_RHO);
    fprintf(fp, "# IRQ MMIO base: 0x%08x (%s)\n\n", irq_ctx.phys_base,
            irq_mapped ? "mapped" : "unavailable");
    fflush(fp);

    if (!irq_mapped)
        printf("[warn] SoCurity IRQ MMIO not mapped; continuing with monitor-only flow\n");

    /* Initial snapshot */
    esp_monitor(mon_args, &vals_start);
    gettimeofday(&tv_prev, NULL);
    usleep(USLEEP_USECS);

    while (1) {
        esp_monitor(mon_args, &vals_curr);
        gettimeofday(&tv_curr, NULL);

        vals_diff  = esp_monitor_diff(vals_start, vals_curr);
        vals_start = vals_curr;

        uint64_t elapsed_usec =
            (uint64_t)(tv_curr.tv_sec  - tv_prev.tv_sec)  * 1000000 +
            (uint64_t)(tv_curr.tv_usec - tv_prev.tv_usec);
        tv_prev = tv_curr;

        /* Feature extraction + OCSVM */
        double feat_raw[FEATURE_COUNT];
        double feat_scaled[FEATURE_COUNT];
        get_features(&vals_diff, feat_raw);
        min_max_scale_d(feat_raw, feat_scaled);

        double ocsvm_sc   = ocsvm_score_d(feat_scaled);
        int    is_anomaly = (ocsvm_sc <= 0.0) ? 1 : 0;

        total_samples++;
        if (is_anomaly) total_anomalies++;

        uint32_t irq_iteration = 0;
        int irq_event = socurity_irq_poll_and_clear(&irq_ctx, &irq_iteration);
        if (irq_event)
            total_irq_events++;

        /* Write to file */
        fprintf(fp, "--- Sample %llu  elapsed_usec=%llu ---\n",
                (unsigned long long)total_samples,
                (unsigned long long)elapsed_usec);

        fprintf(fp, "[RAW MONITORS]\n");
        esp_monitor_print(mon_args, vals_diff, fp);

        print_security_features(fp, &vals_diff);

        fprintf(fp, "[OCSVM]\n");
        fprintf(fp, "  score=%.6f  label=%s\n",
                ocsvm_sc, is_anomaly ? "ANOMALY" : "regular");

        fprintf(fp, "[SOCURITY_IRQ]\n");
        fprintf(fp, "  mapped=%d  event=%d  iteration=%u\n",
                irq_mapped, irq_event, irq_iteration);

        double rate = 100.0 * (double)total_anomalies / (double)total_samples;
        fprintf(fp, "[STATS]\n");
        fprintf(fp, "  total=%llu  anomalies=%llu  irq_events=%llu  rate=%.1f%%\n\n",
                (unsigned long long)total_samples,
                (unsigned long long)total_anomalies,
                (unsigned long long)total_irq_events,
                rate);
        fflush(fp);

        /* Console line — always on anomaly, every 10 samples otherwise */
        if (is_anomaly)
            printf("[!] ANOMALY  score=%+.4f  sample=%llu  rate=%.1f%%\n",
                   ocsvm_sc, (unsigned long long)total_samples, rate);
        else if (total_samples % 10 == 0)
            printf("[ ] normal   score=%+.4f  sample=%llu  rate=%.1f%%\n",
                   ocsvm_sc, (unsigned long long)total_samples, rate);

        if (irq_event)
            printf("[irq] SoCurity alert iteration=%u (cleared)\n", irq_iteration);

        usleep(USLEEP_USECS);
    }

    socurity_irq_close(&irq_ctx);
    fclose(fp);
    return 0;
}
