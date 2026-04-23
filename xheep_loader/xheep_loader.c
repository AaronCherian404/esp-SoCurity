/*
 * xheep_loader.c
 *
 * Linux userspace utility to DMA-load OCSVM firmware onto the x-heep (Ibex)
 * accelerator tile and release it to execute.
 *
 * This is the Linux counterpart of the bare-metal systest.c Phase-3 flow.
 * It uses /dev/mem to access XHEEP APB registers and the ESP contig_alloc
 * driver (/dev/contig) for DMA-visible memory.
 *
 * Usage:
 *   xheep_loader <firmware.bin>
 *
 * Build:
 *   See Makefile.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* =========================================================================
 * XHEEP APB register map (same offsets as systest.c)
 * ========================================================================= */
#define XHEEP_APB_BASE       0x60010000UL
#define XHEEP_REG_SPAN       0x200UL        /* 512 bytes */

#define XHEEP_CMD_REG        0x00UL
#define XHEEP_STATUS_REG     0x04UL
#define XHEEP_PT_ADDRESS_REG 0x0CUL
#define XHEEP_PT_NCHUNK_REG  0x10UL
#define XHEEP_PT_SHIFT_REG   0x14UL
#define XHEEP_PT_NCHUNK_MAX  0x18UL
#define XHEEP_COHERENCE_REG  0x20UL
#define XHEEP_SRC_OFFSET_REG 0x28UL
#define XHEEP_DST_OFFSET_REG 0x2CUL
#define XHEEP_CODE_SIZE_REG  0x40UL
#define XHEEP_FETCH_ADDR_REG 0x44UL
#define XHEEP_FETCH_TRIG_REG 0x48UL
#define XHEEP_EXIT_TRIG_REG  0x4CUL

#define ACC_CMD_START  (1U << 0)
#define ACC_STAT_DONE  (1U << 1)
#define ACC_STAT_ERR   (1U << 2)
#define ACC_COH_NONE   0U

/* =========================================================================
 * /dev/contig ioctl interface (from ESP contig_alloc driver)
 *
 * If the ESP contig_alloc driver is not available, we fall back to a
 * simple aligned malloc + physical address via /proc/self/pagemap.
 * ========================================================================= */
#include <sys/ioctl.h>

#define CONTIG_IOC_MAGIC  'k'
#define CONTIG_IOC_ALLOC  _IOW(CONTIG_IOC_MAGIC, 0, unsigned long)
#define CONTIG_IOC_FREE   _IOW(CONTIG_IOC_MAGIC, 1, unsigned long)
#define CONTIG_IOC_PHYS   _IOR(CONTIG_IOC_MAGIC, 2, unsigned long)

/* =========================================================================
 * MMIO helpers
 * ========================================================================= */
static volatile uint32_t *g_xheep_regs;

static inline uint32_t xheep_rd(uint32_t off) {
    return g_xheep_regs[off >> 2];
}

static inline void xheep_wr(uint32_t off, uint32_t val) {
    g_xheep_regs[off >> 2] = val;
}

/* =========================================================================
 * Physical address lookup via /proc/self/pagemap
 * ========================================================================= */
static uint64_t virt_to_phys(void *vaddr)
{
    int fd;
    uint64_t entry;
    off_t offset;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0)
        return 0;

    offset = ((uintptr_t)vaddr / sysconf(_SC_PAGESIZE)) * sizeof(uint64_t);
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        close(fd);
        return 0;
    }

    if (read(fd, &entry, sizeof(entry)) != sizeof(entry)) {
        close(fd);
        return 0;
    }
    close(fd);

    if (!(entry & (1ULL << 63)))   /* page not present */
        return 0;

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    return pfn * sysconf(_SC_PAGESIZE) + ((uintptr_t)vaddr % sysconf(_SC_PAGESIZE));
}

/* =========================================================================
 * Read firmware binary from file
 * ========================================================================= */
static uint8_t *read_firmware(const char *path, size_t *out_size)
{
    struct stat st;
    FILE *f;
    uint8_t *buf;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "Error: cannot stat '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    *out_size = (size_t)st.st_size;
    buf = (uint8_t *)malloc(*out_size);
    if (!buf) {
        fprintf(stderr, "Error: malloc(%zu) failed\n", *out_size);
        return NULL;
    }

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
        free(buf);
        return NULL;
    }
    if (fread(buf, 1, *out_size, f) != *out_size) {
        fprintf(stderr, "Error: short read from '%s'\n", path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    return buf;
}

/* =========================================================================
 * Main — DMA-load firmware, release Ibex
 * ========================================================================= */
int main(int argc, char *argv[])
{
    int dev_mem_fd;
    void *xheep_map;
    uint8_t *fw_data;
    size_t fw_size;
    int contig_fd;
    void *dma_buf = NULL;
    unsigned long dma_phys = 0;
    uint32_t *ptable = NULL;
    unsigned long ptable_phys = 0;
    uint32_t nchunk_max, status;
    uint32_t fw_words;
    unsigned long page_size;
    int use_contig = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <firmware.bin>\n", argv[0]);
        return 1;
    }

    /* ---- Load firmware binary from file ---- */
    fw_data = read_firmware(argv[1], &fw_size);
    if (!fw_data)
        return 1;

    fw_words = (uint32_t)(fw_size / 4U);
    if (fw_size % 4)
        fw_words++;

    printf("[xheep_loader] firmware: %s (%zu bytes, %u words)\n",
           argv[1], fw_size, fw_words);

    /* ---- Map XHEEP APB registers via /dev/mem ---- */
    dev_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (dev_mem_fd < 0) {
        fprintf(stderr, "Error: cannot open /dev/mem: %s\n", strerror(errno));
        free(fw_data);
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE);
    xheep_map = mmap(NULL, XHEEP_REG_SPAN, PROT_READ | PROT_WRITE,
                     MAP_SHARED, dev_mem_fd,
                     XHEEP_APB_BASE & ~(page_size - 1));
    if (xheep_map == MAP_FAILED) {
        fprintf(stderr, "Error: mmap XHEEP APB failed: %s\n", strerror(errno));
        close(dev_mem_fd);
        free(fw_data);
        return 1;
    }
    g_xheep_regs = (volatile uint32_t *)((uint8_t *)xheep_map +
                    (XHEEP_APB_BASE & (page_size - 1)));

    nchunk_max = xheep_rd(XHEEP_PT_NCHUNK_MAX);
    printf("[xheep_loader] PT_NCHUNK_MAX = %u\n", nchunk_max);

    if (nchunk_max == 0) {
        fprintf(stderr, "Error: scatter-gather DMA not available (PT_NCHUNK_MAX=0)\n");
        goto cleanup;
    }

    /* ---- Allocate DMA-visible memory for firmware image ---- */
    contig_fd = open("/dev/contig", O_RDWR);
    if (contig_fd >= 0) {
        /* Use ESP contig_alloc driver */
        unsigned long alloc_size = fw_size;
        if (ioctl(contig_fd, CONTIG_IOC_ALLOC, &alloc_size) == 0) {
            dma_buf = mmap(NULL, fw_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, contig_fd, 0);
            if (dma_buf != MAP_FAILED) {
                ioctl(contig_fd, CONTIG_IOC_PHYS, &dma_phys);
                use_contig = 1;
                printf("[xheep_loader] contig_alloc: virt=%p phys=0x%lx size=%zu\n",
                       dma_buf, dma_phys, fw_size);
            } else {
                dma_buf = NULL;
            }
        }
        if (!use_contig)
            close(contig_fd);
    }

    if (!use_contig) {
        /* Fallback: use mmap'd anonymous memory + pagemap lookup */
        size_t alloc_sz = (fw_size + page_size - 1) & ~(page_size - 1);
        dma_buf = mmap(NULL, alloc_sz, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
        if (dma_buf == MAP_FAILED) {
            fprintf(stderr, "Error: cannot allocate DMA buffer: %s\n", strerror(errno));
            goto cleanup;
        }

        /* Touch pages to fault them in */
        memset(dma_buf, 0, alloc_sz);

        dma_phys = (unsigned long)virt_to_phys(dma_buf);
        if (dma_phys == 0) {
            fprintf(stderr, "Error: cannot resolve physical address (need root for pagemap)\n");
            goto cleanup;
        }
        printf("[xheep_loader] anon mmap: virt=%p phys=0x%lx size=%zu\n",
               dma_buf, dma_phys, fw_size);
    }

    /* Copy firmware into DMA buffer */
    memcpy(dma_buf, fw_data, fw_size);

    /* ---- Set up page table (single 1 MB chunk) ---- */
    /* Allocate page table entry in same DMA region or separately */
    {
        size_t pt_alloc = page_size;
        void *pt_map = mmap(NULL, pt_alloc, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
        if (pt_map == MAP_FAILED) {
            fprintf(stderr, "Error: cannot allocate page table: %s\n", strerror(errno));
            goto cleanup;
        }
        memset(pt_map, 0, pt_alloc);
        ptable = (uint32_t *)pt_map;
        ptable[0] = (uint32_t)dma_phys;

        ptable_phys = (unsigned long)virt_to_phys(ptable);
        if (ptable_phys == 0) {
            fprintf(stderr, "Error: cannot resolve PT physical address\n");
            goto cleanup;
        }
        printf("[xheep_loader] page table: virt=%p phys=0x%lx entry[0]=0x%08x\n",
               (void *)ptable, ptable_phys, ptable[0]);
    }

    /* ---- Step 1: Configure and trigger DMA fetch ---- */
    printf("[xheep_loader] Configuring DMA fetch...\n");

    xheep_wr(XHEEP_COHERENCE_REG,  ACC_COH_NONE);
    xheep_wr(XHEEP_PT_ADDRESS_REG, (uint32_t)ptable_phys);
    xheep_wr(XHEEP_PT_NCHUNK_REG,  1U);
    xheep_wr(XHEEP_PT_SHIFT_REG,   20U);  /* 2^20 = 1 MB */
    xheep_wr(XHEEP_SRC_OFFSET_REG, 0U);
    xheep_wr(XHEEP_DST_OFFSET_REG, 0U);

    xheep_wr(XHEEP_CODE_SIZE_REG,  fw_words);
    xheep_wr(XHEEP_FETCH_ADDR_REG, 0U);
    xheep_wr(XHEEP_FETCH_TRIG_REG, 1U);
    xheep_wr(XHEEP_EXIT_TRIG_REG,  0U);   /* hold Ibex in exit-loop */

    xheep_wr(XHEEP_CMD_REG, ACC_CMD_START);
    printf("[xheep_loader] DMA fetch started (%u words -> Ibex SRAM)\n", fw_words);

    /* Poll for completion */
    {
        unsigned int polls = 0;
        do {
            status = xheep_rd(XHEEP_STATUS_REG);
            polls++;
        } while (!(status & ACC_STAT_DONE) && polls < 2000000U);

        if (!(status & ACC_STAT_DONE)) {
            fprintf(stderr, "Error: DMA fetch timeout (polls=%u status=0x%02x)\n",
                    polls, status);
            if (status & ACC_STAT_ERR)
                fprintf(stderr, "  ACC error flag set\n");
            goto cleanup;
        }
        printf("[xheep_loader] DMA fetch complete (polls=%u)\n", polls);
    }

    xheep_wr(XHEEP_CMD_REG, 0U);  /* clear done */

    /* ---- Step 2: Release Ibex ---- */
    xheep_wr(XHEEP_CODE_SIZE_REG,  0U);
    xheep_wr(XHEEP_FETCH_TRIG_REG, 0U);
    xheep_wr(XHEEP_EXIT_TRIG_REG,  1U);   /* release exit-loop */

    xheep_wr(XHEEP_CMD_REG, ACC_CMD_START);
    printf("[xheep_loader] Ibex released — OCSVM anomaly detector running\n");
    printf("[xheep_loader] Done. Monitor alerts via background_monitor or dmesg.\n");

    /* Don't free DMA memory — it must stay mapped while Ibex might still read it */
    free(fw_data);
    /* Leave /dev/mem fd and mappings open — the kernel holds them */
    return 0;

cleanup:
    if (xheep_map && xheep_map != MAP_FAILED)
        munmap(xheep_map, XHEEP_REG_SPAN);
    if (dev_mem_fd >= 0)
        close(dev_mem_fd);
    if (use_contig && contig_fd >= 0) {
        unsigned long dummy = 0;
        ioctl(contig_fd, CONTIG_IOC_FREE, &dummy);
        close(contig_fd);
    }
    free(fw_data);
    return 1;
}
