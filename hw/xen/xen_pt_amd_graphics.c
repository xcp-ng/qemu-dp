#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/main-loop.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "xen_pt.h"
#include "xen-host-pci-device.h"
#include "gim_ioctl.h"

#include <sys/ioctl.h>

int xen_pt_register_amd_vf_region(XenPCIPassthroughState *s);
int xen_pt_unregister_amd_vf_region(XenPCIPassthroughState *s);

#define PT_LOG(_f, _a...)  xen_pt_log(NULL, "%s: " _f, __func__, ##_a)

//#define VERBOSE_LOGGING

#ifdef VERBOSE_LOGGING
#define VERBOSE_LOG(_f, _a...)  xen_pt_log(NULL, "%s: " _f, __func__, ##_a)
#else
#define VERBOSE_LOG(_f, _a...)
#endif

#define BLOCK_MMIO 		1
#define DO_NOT_BLOCK_MMIO	2

static int amd_default_mmio_behavior = BLOCK_MMIO;
//#define MMIO_LOGGING
//#define MMIO_LIST

#ifdef MMIO_LOGGING
static int MMIO_count;
#endif

static int good_MMIO_count;
static int bad_MMIO_count;

static uint32_t pt_amd_mmio_bar_num;
static pcibus_t pt_amd_mmio_bar_maddr;
static pcibus_t pt_amd_mmio_bar_gaddr;
static pcibus_t pt_amd_mmio_bar_size;
static void *pt_amd_mmio_bar_ptr;

static Notifier pt_amd_exit_notifier;

static char gim_file_name[] = "/dev/gim";
static char gim_sysfs_dir[] = "/sys/devices/virtual/sriov/gim/";
static char *gim_sysfs_node;
static int gim_sysfs_fd;

/*
 * Structure to maintain a list of MMIO registers and whether they are
 * white or black listed
 */
struct emulated_mmio {
    uint32_t offset;
    uint32_t valid;
} *amd_emulated_mmio = NULL;

static uint32_t amd_num_emulated_mmio;
static uint32_t amd_emulated_mmio_size;

#define MMIO_SIZE_INCREMENT 32

static int amd_mmio_is_xen_mapped;

struct mmio_counter {
    uint32_t offset;
    uint32_t read_count;
    uint32_t write_count;
};

static int max_bad_mmios;
#define BAD_MMIO_INC 32

static struct mmio_counter *bad_mmios;
static int bad_mmio_count;

#define MAX_PASSTHROUGH_RANGES 16

struct passthrough_range {
    uint32_t ebase;
    uint32_t esize;
} amd_passthrough_ranges[MAX_PASSTHROUGH_RANGES];

static bool pt_trap_needed = true;

static bool pt_ati_get_mmio_bar_index(XenHostPCIDevice *d)
{
    bool found = false;
    unsigned int i;

    /*
     * Find the MMIO BAR.  The MMIO has the attributes of MEMORY and
     * non-prefetch.
     *
     * PCI_NUM_REGIONS is the 6 BARs plus the ROM expansion BAR (==7)
     */
    for (i = 0; i < PCI_NUM_REGIONS - 1 && !found; ++i) {
        XenHostPCIIORegion *r = &d->io_regions[i];

        if (r->base_addr == 0 || r->size == 0) {
            continue;
        }

        /*
         * Look for a non-prefetch BAR in memory space
         */
        if (r->type & XEN_HOST_PCI_REGION_TYPE_IO ||
            r->type & XEN_HOST_PCI_REGION_TYPE_PREFETCH) {
            continue;
        }

        pt_amd_mmio_bar_num = i;

        /* MMIO can't be 64 bit BAR. */
        found = !(r->type & XEN_HOST_PCI_REGION_TYPE_MEM_64);
    }

    return found;
}

/*
 * Map a Host physical address to a virtual address
 */
static void pt_amd_mmap(void)
{
    int fd;

    if ((fd = open("/dev/mem", O_RDWR)) < 0) {
        PT_LOG("Serious ERROR: Failed to open /dev/mem\n");
        return;
    }

    pt_amd_mmio_bar_ptr = mmap64(0, pt_amd_mmio_bar_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, pt_amd_mmio_bar_maddr);

    close(fd);
}

static void pt_amd_munmap(void)
{
    if (!pt_amd_mmio_bar_ptr)
        return;

    munmap(pt_amd_mmio_bar_ptr, pt_amd_mmio_bar_size);
}

static void dump_bad_mmio(void)
{
    unsigned int i;

    PT_LOG("%d bad MMIO accesses detected\n", bad_mmio_count);

    for (i = 0; i < bad_mmio_count; ++i) {
        PT_LOG("MMIO offset 0x%08x.  %d bad READs, %d bad WRITEs\n",
               bad_mmios[i].offset, bad_mmios[i].read_count,
               bad_mmios[i].write_count);
    }
}

static void pt_amd_set_single_mapping(uint32_t gaddr, uint64_t maddr,
                                      uint32_t size, int op)
{
    const char *opstr = DPCI_REMOVE_MAPPING ? "REMOVE" : "ADD";

    if (xc_domain_memory_mapping(xen_xc, xen_domid,
                                 XEN_PFN(gaddr),
                                 XEN_PFN(maddr),
                                 XEN_PFN(size + XC_PAGE_SIZE - 1),
                                 op) < 0) {
        PT_LOG("%s mapping failed for base 0x%08x and size 0x%04x (%s)\n",
               opstr, gaddr, size, strerror(errno));
    }
}

static void add_passthrough_range(uint32_t offset, uint32_t len)
{
    int i;

    for (i = 0;
         i < MAX_PASSTHROUGH_RANGES && amd_passthrough_ranges[i].esize != 0;
         ++i);

    if (i == MAX_PASSTHROUGH_RANGES) {
        PT_LOG("Out of entries in amd_passthrough_ranges[]\n");
        return;
    }

    amd_passthrough_ranges[i].ebase = offset;
    amd_passthrough_ranges[i].esize = len;

    VERBOSE_LOG("Create new range entry 0x%04x to 0x%04x\n",
                amd_passthrough_ranges[i].ebase,
                amd_passthrough_ranges[i].ebase +
                amd_passthrough_ranges[i].esize);
    pt_trap_needed = false;
}

static void clear_passthrough_ranges(void)
{
    int i;

    pt_trap_needed = true;
    for (i = 0; i < MAX_PASSTHROUGH_RANGES; ++i) {
        amd_passthrough_ranges[i].esize = 0;
        amd_passthrough_ranges[i].ebase = 0;
    }
}

static void remove_from_ranges(uint32_t offset)
{
    int i;
    uint32_t page_offset;
    uint32_t first_page, last_page;

    VERBOSE_LOG("Check if 0x%04x falls in an existing range\n", offset);

    /* Check if the offset falls into a pre-defined range */
    for (i = 0;
         i < MAX_PASSTHROUGH_RANGES && amd_passthrough_ranges[i].esize != 0;
         ++i) {
        if (offset >= amd_passthrough_ranges[i].ebase &&
            offset < (amd_passthrough_ranges[i].ebase +
                      amd_passthrough_ranges[i].esize)) {
            VERBOSE_LOG("Offset 0x%04x falls in range from 0x%04x to 0x%04x\n",
                        offset, amd_passthrough_ranges[i].ebase,
                        amd_passthrough_ranges[i].ebase +
                        amd_passthrough_ranges[i].esize - 1);

            page_offset = offset / XC_PAGE_SIZE;

            if (amd_passthrough_ranges[i].esize == XC_PAGE_SIZE) {
                VERBOSE_LOG("Range is only a single page. "
                            "Need to shuffle other pages up\n");

                /* Shift all entries up by one */
                amd_passthrough_ranges[i].esize = 0;

                for (;
                     (i + 1) < MAX_PASSTHROUGH_RANGES &&
                     amd_passthrough_ranges[i + 1].esize != 0;
                     ++i) {
                    amd_passthrough_ranges[i].esize =
                        amd_passthrough_ranges[i + 1].esize;
                    amd_passthrough_ranges[i].ebase =
                        amd_passthrough_ranges[i + 1].ebase;
                }

                amd_passthrough_ranges[i].esize = 0;
            } else {
                first_page = amd_passthrough_ranges[i].ebase / XC_PAGE_SIZE;
                last_page = (amd_passthrough_ranges[i].ebase +
                             amd_passthrough_ranges[i].esize -
                             1) / XC_PAGE_SIZE;

                VERBOSE_LOG("Offset is on page 0x%d in a range "
                            "covering pages 0x%0x to 0x%0x\n",
                             page_offset, first_page, last_page);

                if (page_offset == first_page) {
                    VERBOSE_LOG("offset is on first page.  Move base up\n");

                    amd_passthrough_ranges[i].ebase += XC_PAGE_SIZE;
                    amd_passthrough_ranges[i].esize -= XC_PAGE_SIZE;

                    VERBOSE_LOG("Range entry becomes 0x%04x to 0x%04x\n",
                                amd_passthrough_ranges[i].ebase,
                                amd_passthrough_ranges[i].ebase +
                                amd_passthrough_ranges[i].esize);
                } else if (page_offset == last_page) {
                    VERBOSE_LOG("offset is one last page, "
                                "make the range smaller\n");

                    amd_passthrough_ranges[i].esize -= XC_PAGE_SIZE;

                    VERBOSE_LOG("Range entry becomes 0x%04x to 0x%04x\n",
                                amd_passthrough_ranges[i].ebase,
                                amd_passthrough_ranges[i].ebase +
                                amd_passthrough_ranges[i].esize);
                } else {
                    VERBOSE_LOG("Offset is in the middle of a range, "
                                "need to split it.\n");

                    amd_passthrough_ranges[i].esize =
                        (page_offset - first_page) * XC_PAGE_SIZE;

                    VERBOSE_LOG("Range entry becomes 0x%04x to 0x%04x\n",
                                amd_passthrough_ranges[i].ebase,
                                amd_passthrough_ranges[i].ebase +
                                amd_passthrough_ranges[i].esize);

                    add_passthrough_range((page_offset + 1) * XC_PAGE_SIZE,
                                          (last_page -
                                           (page_offset +
                                            1)) * XC_PAGE_SIZE);
                }
            }
            break;
        }
    }
}

static void pt_amd_set_mapping(int op)
{
    unsigned int i;

    VERBOSE_LOG("Update the Xen Mapping\n");

    for (i = 0;
         i < MAX_PASSTHROUGH_RANGES && amd_passthrough_ranges[i].esize != 0;
         ++i) {
        uint64_t maddr;
        uint32_t gaddr;
        uint32_t base_delta;
        uint32_t size;

        base_delta = amd_passthrough_ranges[i].ebase;
        size = amd_passthrough_ranges[i].esize;
        if (size + base_delta > pt_amd_mmio_bar_size)
            size = pt_amd_mmio_bar_size - base_delta;

        maddr = pt_amd_mmio_bar_maddr + base_delta;
        gaddr = pt_amd_mmio_bar_gaddr + base_delta;
        pt_amd_set_single_mapping(gaddr, maddr, size, op);
    }

    if (i == 0) {
        PT_LOG("WARNING: No ranges defined for op = %s\n",
               op == DPCI_REMOVE_MAPPING ? "REMOVE" : "ADD");
    }

    VERBOSE_LOG("Xen Mapping complete\n");
}

static void pt_amd_vf_trap_mmio(void)
{
    VERBOSE_LOG("Received a request to start MMIO TRAPPING - "
                "needed=%d, mmio_is_xen_mapped=%d\n",
                pt_trap_needed, amd_mmio_is_xen_mapped);

    if (amd_mmio_is_xen_mapped) {
        VERBOSE_LOG("Remove Xen mapping so that readl/writel are called\n");
        VERBOSE_LOG("Trap all MMIO accesses to readl() and writel()\n");
        VERBOSE_LOG("Trap guest physical addr %p access on BAR%d. "
                    "Mapped to local ptr %p in domain %d\n",
                    (void *)pt_amd_mmio_bar_maddr, pt_amd_mmio_bar_num,
                    pt_amd_mmio_bar_ptr, xen_domid);
        amd_mmio_is_xen_mapped = 0;
        pt_amd_set_mapping(DPCI_REMOVE_MAPPING);
    } else {
        VERBOSE_LOG("MMIO Trapping is already enabled. "
                    "Therefore it was not enabled again\n");
    }
}

static void pt_amd_vf_passthru_mmio(void)
{
    VERBOSE_LOG("Received a request to stop MMIO TRAPPING - "
                "needed=%d, mmio_is_mapped=%d\n",
                pt_trap_needed, amd_mmio_is_xen_mapped);

    if ((!pt_trap_needed && !amd_mmio_is_xen_mapped)) {
        VERBOSE_LOG("Allow striaght pass through of guest accessing MMIO\n");
        amd_mmio_is_xen_mapped = 1;
        pt_amd_set_mapping(DPCI_ADD_MAPPING);
    } else {
        VERBOSE_LOG("Trapping was not enabled. "
                    "Therefore nothing to unregister\n");
    }
}

/*
 * Block a range of MMIO offsets
 * Currently only support blocking the entire MMIO BAR range
 * Blocking of sub-ranges not require therefore not supported.
 *
 * Currently the only valid range can be specified as "BA"
 */
static void pt_block_mmio(char *range)
{
    void *ptr;
    char *range_ptr;
    uint32_t val;
    int num_mmio = sizeof(uint32_t);

    PT_LOG("Request from GIM to BLOCK MMIO access \"%s\"\n", range);

    /* If BLOCK ALL then set the default behaviour */
    if (range[0] == 'A') {
        PT_LOG("Block ALL MMIO range\n");
        pt_amd_vf_trap_mmio();
        amd_num_emulated_mmio = 0;
        bad_mmio_count = 0;
        clear_passthrough_ranges();
        amd_default_mmio_behavior = BLOCK_MMIO;
    } else {
        /* Individual MMIO range was specified */
        val = strtoul(range, &range_ptr, 0);

        if (*range_ptr == '/') { /* Range specified */
            num_mmio = strtoul(&range_ptr[1], NULL, 0);
        }

        /*
         * If the range is a multiple of an MMIO page then add the
         * entire PAGE
         */
        num_mmio /= sizeof(uint32_t);

        PT_LOG("Remove %d consecutive MMIO offsets from "
               "the valid emulated MMIO list\n", num_mmio);

        while (num_mmio) {
            /* Check if the emulated MMIO list is large enough */
            if (amd_num_emulated_mmio >= amd_emulated_mmio_size) {
                ptr = realloc(amd_emulated_mmio,
                              sizeof(struct emulated_mmio) *
                              (amd_emulated_mmio_size +
                               MMIO_SIZE_INCREMENT));
                if (ptr != NULL) {
                    amd_emulated_mmio = ptr;
                    amd_emulated_mmio_size += MMIO_SIZE_INCREMENT;
                } else {
                    PT_LOG("Cannot add %s to the valid MMIO list\n",
                           range);
                    PT_LOG("Failed to increase the size of the list "
                           "from %d entries to %d entries\n",
                           amd_emulated_mmio_size,
                           amd_emulated_mmio_size + MMIO_SIZE_INCREMENT);
                    break;
                }
            }
            remove_from_ranges(val);
            amd_emulated_mmio[amd_num_emulated_mmio].offset = val;
            amd_emulated_mmio[amd_num_emulated_mmio].valid = 0;
            ++amd_num_emulated_mmio;
            --num_mmio;
            val += 4; /* Next MMIO offset */
        }
    }
}

/*
 * Unblock an MMIO range
 *
 * Valid syntax is <offset>[/<range>]
 * where offset is an MMIO offset and range is the offset number of bytes to include in the range
 *
 * Range and offset can be either hex (0x...) format or decimal format.
 *
 * for example 0x5100/40 specifies a starting offset of 0x5100 for a length of 40 bytes (or 10 DWORDS)
 */
static void pt_unblock_mmio(char *range)
{
    void *ptr;
    char *range_ptr;
    uint32_t val;
    int num_mmio = sizeof(uint32_t);

    PT_LOG("Request from GIM to UNBLOCK MMIO access \"%s\"\n", range);

    if (range[0] == 'A') {
        PT_LOG("Unblock ALL MMIO range\n");
        clear_passthrough_ranges();
        add_passthrough_range(0, pt_amd_mmio_bar_size);
        amd_default_mmio_behavior = DO_NOT_BLOCK_MMIO;
    } else {
        val = strtoul(range, &range_ptr, 0);

        if (*range_ptr == '/') { /* Range specified */
            num_mmio = strtoul(&range_ptr[1], NULL, 0);
        }

        num_mmio /= sizeof(uint32_t);

        PT_LOG("Add %d consecutive MMIO offsets to "
               "the valid emulated MMIO list\n", num_mmio);

        while (num_mmio) {
            if ((amd_num_emulated_mmio) >= amd_emulated_mmio_size) {
                ptr = realloc(amd_emulated_mmio,
                              sizeof(struct emulated_mmio) *
                              (amd_emulated_mmio_size +
                               MMIO_SIZE_INCREMENT));
                if (ptr != NULL) {
                    amd_emulated_mmio = ptr;
                    amd_emulated_mmio_size += MMIO_SIZE_INCREMENT;
                } else {
                    PT_LOG("Cannot add %s to the valid MMIO list\n",
                           range);
                    PT_LOG("Failed to increase the size of the list "
                           "from %d entries to %d entries\n",
                           amd_emulated_mmio_size,
                           amd_emulated_mmio_size + MMIO_SIZE_INCREMENT);
                    break;
                }
            }
            amd_emulated_mmio[amd_num_emulated_mmio].offset = val;
            amd_emulated_mmio[amd_num_emulated_mmio].valid = 1;
            ++amd_num_emulated_mmio;
            --num_mmio;
            val += 4; /* Next MMIO offset  */
        }
    }
}

static void remove_spaces(char *cmd)
{
    int in, out;

    in = 0;
    out = 0;

    for (in = 0; cmd[in] != '\0'; ++in) {
        if (!isspace((unsigned char) cmd[in])) {
            cmd[out] = cmd[in];
            ++out;
        }
    }

    cmd[out] = '\0';
}

static void pt_execute_token(char *cmd)
{
    if (!strncmp(cmd, "B", 1)) {	/* 'B' for BLOCK MMIO */
        pt_block_mmio(&cmd[1]);
    } else if (!strncmp(cmd, "U", 1)) {	/* 'U' for UNBLOCK MMIO */
        pt_unblock_mmio(&cmd[1]);
    } else {
        PT_LOG("Unknown command \"%s\"\n", cmd);
    }
}

static void notify_gim(int msg)
{
    int ioctl_fd;

    ioctl_fd = open(gim_file_name, O_RDWR);
    if (ioctl_fd == -1) {
        PT_LOG("Failed to open %s (%d)\n", gim_file_name, errno);
        return;
    }
    VERBOSE_LOG("Opened device %s\n", gim_file_name);

    if (ioctl(ioctl_fd, msg) == -1) {
        PT_LOG("IOCTL call failed\n");
        close(ioctl_fd);
        return;
    }
    VERBOSE_LOG("IOCTL was successful\n");

    close(ioctl_fd);
}


static void pt_execute(char *cmd)
{
    char *token;
    char *save_ptr;

    remove_spaces(cmd);
    PT_LOG("GIM command = \"%s\"\n", cmd);

    token = strtok_r(cmd, ",", &save_ptr);

    while (token != NULL) {
        pt_execute_token(token);

        token = strtok_r(NULL, ",", &save_ptr);
    }

    if (amd_passthrough_ranges[0].esize != 0) {
        PT_LOG("There is a MMIO PASSTHRU range defined\n");
#ifndef MMIO_LOGGING
        pt_amd_vf_passthru_mmio();
#endif
        PT_LOG("MMIO_IS_PASS_THROUGH\n");
        notify_gim(GIM_IOCTL_MMIO_IS_PASS_THROUGH);
    } else {
        PT_LOG("MMIO_IS_BLOCKED\n");
        notify_gim(GIM_IOCTL_MMIO_IS_BLOCKED);
    }

    PT_LOG("%d good MMIOs, %d bad MMIOs\n",
           good_MMIO_count, bad_MMIO_count);
}

/*
 * GIM writes to sysfs file will land in this callback function
 * opaque contains the file descriptor to read
 */

#define MAX_SYSFS_READ 4095

static void pt_amd_exception(void *opaque)
{
    int fd;
    int rc;
    char sysfs_buf[MAX_SYSFS_READ + 1];

    fd = (int)(intptr_t)opaque;

    VERBOSE_LOG("received an exception: data = %d\n", fd);

    memset(sysfs_buf, 0, sizeof(sysfs_buf));
    rc = read(fd, sysfs_buf, MAX_SYSFS_READ);

    VERBOSE_LOG("Read returns %d\n", rc);

    if (rc > MAX_SYSFS_READ) {
        PT_LOG("Message from GIM is too large for %d sized buffer\n",
               MAX_SYSFS_READ);
    }

    if (rc < 0) { /* Error with pipe or no data */
        return;
    }

    if (rc == 0) {
        unsigned int i;

        close(fd);

        fd = open(gim_sysfs_node, O_RDONLY);
        if (fd < 0) {
            PT_LOG("Failed to reopen %s (%d)\n", gim_sysfs_node, errno);
            g_free(gim_sysfs_node);
            qemu_set_fd_handler3(gim_sysfs_fd, NULL, NULL, NULL, NULL);
            return;
        }
        VERBOSE_LOG("Reopening \"%s\" returns fd = %d\n", gim_sysfs_node, fd);

        memset(sysfs_buf, 0, sizeof(sysfs_buf));

        i = 0;
        do {
            rc = read(fd, &sysfs_buf[i], MAX_SYSFS_READ - i);
            VERBOSE_LOG("Read returns %d\n", rc);
            if (rc > 0)
                i += rc;
        } while (rc > 0);
    }

    VERBOSE_LOG("GIM sent me \"%s\"\n", sysfs_buf);

    dump_bad_mmio();
    pt_execute(sysfs_buf);

    if (fd != (int)(intptr_t)opaque) {
        VERBOSE_LOG("File descriptor has changed. "
                    "Need to re-register QEMU handler\n");
        gim_sysfs_fd = fd;

        qemu_set_fd_handler3((int)(intptr_t)opaque, NULL, NULL, NULL,
                             NULL);
        qemu_set_fd_handler3(gim_sysfs_fd, NULL, NULL, pt_amd_exception,
                             (void *)(intptr_t)gim_sysfs_fd);
    }
}

static void pt_amd_alloc_vf(uint8_t bus, uint8_t dev, uint8_t fn)
{
    struct gim_ioctl_alloc_vf vf;	/* Interface structure for IOCTL */
    int ioctl_fd;
    uint32_t bdf;
    int rc;

    bdf = (bus << 8) + (dev << 3) + fn;

    PT_LOG("Ask GIM to allocate a VF for BDF = %02x:%02x.%x (0x%08x)\n",
           bus, dev, fn, bdf);

    ioctl_fd = open(gim_file_name, O_RDWR);
    if (ioctl_fd == -1) {
        PT_LOG("Failed to open %s (%d)\n", gim_file_name, errno);
        return;
    }

    VERBOSE_LOG("Opened device %s\n", gim_file_name);

    vf.bdf = bdf;

    if ((rc = ioctl(ioctl_fd, GIM_IOCTL_ALLOC_VF, &vf)) == -1) {
        PT_LOG("IOCTL: GIM_IOCTL_ALLOC_VF failed (%d)\n", errno);
    } else {
        VERBOSE_LOG("IOCTL GIM_IOCTL_ALLOC_VF was successful\n");
    }

    if (errno == EAGAIN) {
        VERBOSE_LOG("FB was not cleared but can still continue on\n");
    }

    close(ioctl_fd);

    gim_sysfs_node = g_strdup_printf("%sqemu-%d", gim_sysfs_dir, getpid());
    PT_LOG("Using sysfs node %s\n", gim_sysfs_node);

    gim_sysfs_fd = open(gim_sysfs_node, O_RDONLY);
    if (gim_sysfs_fd < 0) {
        PT_LOG("Failed to open %s (%d)\n", gim_sysfs_node, errno);
        g_free(gim_sysfs_node);
        return;
    }

    qemu_set_fd_handler3(gim_sysfs_fd, NULL, NULL, pt_amd_exception,
			 (void *)(intptr_t)gim_sysfs_fd);
}

static void pt_amd_free_vf(void)
{
    int ioctl_fd;

    PT_LOG("Tell GIM to free the VF\n");

    qemu_set_fd_handler3(gim_sysfs_fd, NULL, NULL, NULL, NULL);

    ioctl_fd = open(gim_file_name, O_RDWR);
    if (ioctl_fd == -1) {
        PT_LOG("Failed to open %s (%d)\n", gim_file_name, errno);
        return;
    }

    VERBOSE_LOG("Opened device %s\n", gim_file_name);

    if (ioctl(ioctl_fd, GIM_IOCTL_FREE_VF) == -1) {
        PT_LOG("IOCTL: GIM_IOCTL_FREE_VF failed (%d)\n", errno);
    } else {
        PT_LOG("IOCTL: GIM_IOCTL_FREE_VF was successful\n");
    }

    close(ioctl_fd);
}

static void pt_amd_enable_mmio(void)
{
    PT_LOG("MMIO BAR is valid\n");

    if (pt_trap_needed) {
        PT_LOG("QEMU trapping is needed, enable readl/writel\n");
        pt_amd_vf_trap_mmio();
    } else {
        PT_LOG("Direct passthrough to MMIO without trapping\n");
        pt_amd_vf_passthru_mmio();
    }
}

static void pt_amd_disable_mmio(void)
{
    PT_LOG("MMIO BAR is not valid\n");

    if (amd_mmio_is_xen_mapped) {
        PT_LOG("MMIO is Xen mapped as passthrough. "
               "Need to remove the mapping\n");
        amd_mmio_is_xen_mapped = 0;
        if (xc_domain_memory_mapping(xen_xc, xen_domid,
                                     XEN_PFN(pt_amd_mmio_bar_gaddr),
                                     XEN_PFN(pt_amd_mmio_bar_maddr),
                                     XEN_PFN(pt_amd_mmio_bar_size),
                                     DPCI_REMOVE_MAPPING) < 0) {
            PT_LOG("Removing mapping failed: %d, %s\n", errno, strerror(errno));
        }

    }
}

static void pt_amd_mmio_bar_map(XenPTBAR *bar,
                                MemoryRegionSection *sec)
{
    struct XenPCIPassthroughState *s = bar->s;
    unsigned int index = bar - &s->bar[0];

    assert(index == pt_amd_mmio_bar_num);

    pt_amd_mmio_bar_gaddr = sec->offset_within_address_space;

    PT_LOG("MMIO BAR: (MEM) %p -> %p [%p]\n",
           (void *)pt_amd_mmio_bar_gaddr,
           (void *)pt_amd_mmio_bar_maddr,
           (void *)pt_amd_mmio_bar_size);

    pt_amd_enable_mmio();
}

static void pt_amd_mmio_bar_unmap(XenPTBAR *bar,
                                    MemoryRegionSection *sec)
{
    struct XenPCIPassthroughState *s = bar->s;
    unsigned int index = bar - &s->bar[0];

    assert(index == pt_amd_mmio_bar_num);

    PT_LOG("MMIO BAR: (MEM) %p -> %p [%p]\n",
           (void *)pt_amd_mmio_bar_gaddr,
           (void *)pt_amd_mmio_bar_maddr,
           (void *)pt_amd_mmio_bar_size);

    pt_amd_disable_mmio();
}

static int can_access_mmio(uint32_t offset, int is_write)
{
    int i;
    void *ptr;

    /*
     * Check if the MMIO offset is emulated or not.
     * If it is emulated then check if it is valid or not valid
     * If it is valid then return 0x1 to indicate that the MMIO
     * access is permitted
     */
    for (i = 0; i < amd_num_emulated_mmio; ++i) {
        if (amd_emulated_mmio[i].offset == offset) {
            if (amd_emulated_mmio[i].valid) {
                ++good_MMIO_count;
                return 1;
            } else {
                ++bad_MMIO_count;
                return 0;
            }
        }
    }

    /*
     * The MMIO offset is not emulated.  Check if the default
     * behavior is to pass through or to block.  If it is passthrough
     * then return 0x1
     */
    if (amd_default_mmio_behavior == DO_NOT_BLOCK_MMIO) {
        ++good_MMIO_count;
        return 1;
    }

    /*
     * The MMIO access is not valid.  This function will return 0x0
     * The remainder of this function is for tracking purposes only.
     * this will log an entry for the bad MMIO access that can be reported
     * in the log file.
     */

    /* Debug tracking summary of Bad MMIO accesses */

    /*
     * Check if the MMIO has previously had a hit.  If an entry already
     * exists then just increment either the invalid read or write attempt.
     */
    for (i = 0; i < bad_mmio_count; ++i) {
        if (bad_mmios[i].offset == offset) {
            if (is_write)
                ++bad_mmios[i].write_count;
            else
                ++bad_mmios[i].read_count;
            return (0);
        }
    }

    /*
     * Check if need to make a new entry in the list.  If the list is not
     * large enough then need to reallocate the list larger by BAD_MMIO_INC
     * number of elements
     */
    if (i >= max_bad_mmios) {
        ptr = realloc(bad_mmios, sizeof(struct mmio_counter) *
                      (max_bad_mmios + BAD_MMIO_INC));
        if (ptr != NULL) {
            bad_mmios = ptr;
            max_bad_mmios += BAD_MMIO_INC;
        } else {
            PT_LOG("Failed to enlarge the bad MMIO list. "
                   "Offset 0x%04x not added\n", offset);
        }
    }

    /*
     * If the realloc worked or the list was already large enough, use
     * another entry
     */
    if (i < max_bad_mmios) {
        bad_mmios[i].offset = offset;
        if (is_write) {
            bad_mmios[i].write_count = 1;;
            bad_mmios[i].read_count = 0;
        } else {
            bad_mmios[i].write_count = 0;;
            bad_mmios[i].read_count = 1;
        }

        bad_mmio_count = i + 1;
    } else {
        printf("No room for more bad MMIOs.  Increase Array size\n");
    }

    return 0;
}

static void pt_amd_mmio_bar_read(XenPTBAR *bar, hwaddr addr,
                                 unsigned int size, uint64_t *value)
{
    uint32_t *mmio;

    if (size != 4) {
        PT_LOG("NOT SUPPORTED: %u byte access to %p\n", size,
               (void *)pt_amd_mmio_bar_gaddr + addr);
        return;
    }

    mmio = (uint32_t *)(pt_amd_mmio_bar_ptr + addr);

#ifdef MMIO_LOGGING
    ++MMIO_count;
    *value = *mmio;
    PT_LOG("[%6d] MMIO_read:  0x%"PRIx64" from 0x%04lx\n", MMIO_count,
           *value, addr);
#else
#ifdef MMIO_LIST
    can_access_mmio(addr, 0); // Call to log the access but allow it anyway
    *value = *mmio;
#else
    /* Normal mode */
    if (can_access_mmio(addr, 0)) {
        *value = *mmio;
    } else {
        PT_LOG("MMIO_read: Invalid READ attempt of MMIO offset 0x%04lx\n",
               addr);
        *value = 0xFFFFFFFF;
    }
#endif
#endif
}

static void pt_amd_mmio_bar_write(XenPTBAR *bar, hwaddr addr,
                                  unsigned int size, uint64_t value)
{
    uint32_t *mmio;

    if (size != 4) {
        PT_LOG("NOT SUPPORTED: %u byte access to %p\n", size,
               (void *)pt_amd_mmio_bar_gaddr + addr);
        return;
    }

    mmio = (uint32_t *)(pt_amd_mmio_bar_ptr + addr);

#ifdef MMIO_LOGGING
    ++MMIO_count;
    PT_LOG("[%6d] MMIO_write: 0x%"PRIx64" to 0x%04lx\n", MMIO_count,
           value, addr);
    *mmio = value;
#else
#ifdef MMIO_LIST
    can_access_mmio(addr, 1);
    *mmio = val;
#else
    if (can_access_mmio(addr, 1)) {
        *mmio = value;
    } else {
        PT_LOG("MMIO_write: Invalid WRITE attempt of 0x%"PRIx64" to "
               "MMIO offset 0x%04lx\n", value, addr);
    }
#endif
#endif
}

static void pt_amd_exit_notify(Notifier *n, void *data)
{
    pt_amd_free_vf();
}

int xen_pt_register_amd_vf_region(XenPCIPassthroughState *s)
{
    XenHostPCIDevice *host_dev = &s->real_device;
    XenPTBAR *bar;

    PT_LOG("Register callback function MMIO BAR changing\n");

    if (!host_dev->is_virtfn)
        return -1;

    /* Find MMIO BAR */

    if (!pt_ati_get_mmio_bar_index(host_dev)) {
        PT_LOG("Could not find MMIO BAR for mapping/trapping\n");
        return -1;
    }

    PT_LOG("MMIO is at BAR%d\n", pt_amd_mmio_bar_num);

    pt_amd_mmio_bar_maddr =
        host_dev->io_regions[pt_amd_mmio_bar_num].base_addr;
    pt_amd_mmio_bar_size = host_dev->io_regions[pt_amd_mmio_bar_num].size;

    PT_LOG("MMIO is at address %p [size = 0x%p]\n",
           (void *)pt_amd_mmio_bar_maddr,
           (void *)pt_amd_mmio_bar_size);

    /* Get a local pointer to the MMIO for Trapping emulation */
    pt_amd_mmap();

    VERBOSE_LOG("Map physical MMIO space to local ptr %p\n",
                pt_amd_mmio_bar_ptr);

    /* Hijack the BAR callbacks */
    bar = &s->bar[pt_amd_mmio_bar_num];

    bar->map = pt_amd_mmio_bar_map;
    bar->unmap = pt_amd_mmio_bar_unmap;
    bar->read = pt_amd_mmio_bar_read;
    bar->write = pt_amd_mmio_bar_write;

    pt_amd_exit_notifier.notify = pt_amd_exit_notify;
    qemu_add_exit_notifier(&pt_amd_exit_notifier);

    /* Tell GIM that we are ready to get started (via IOCTL) by allocating a VF */
    pt_amd_alloc_vf(host_dev->bus, host_dev->dev, host_dev->func);

    return 0;
}

int xen_pt_unregister_amd_vf_region(XenPCIPassthroughState *s)
{
    pt_amd_munmap();

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
