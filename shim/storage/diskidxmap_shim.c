/**
 * diskidxmap shim — compact sd-letter index remapping for nonDT models.
 *
 * PROBLEM
 * HBA controllers enumerate empty SCSI ports before real disks.  Each port
 * gets an sd-letter, then the empty ones are deleted by disks.sh.  The real
 * disks are left with sparse indices (e.g. sdc=2, sdi=8) and appear at DSM
 * slots 3 and 9 instead of 1 and 2.
 *
 * SOLUTION
 * After disks.sh has deleted empty SCSI slots it writes "auto" to
 * /proc/sys/kernel/diskidxmap.  This shim then:
 *   1. Enumerates all live non-empty non-boot SCSI disks in PCI-BDF +
 *      SCSI H:C:T:L order (same stable sort as disks.sh _sorted_sd_disks()).
 *   2. Assigns compact target indices 0, 1, 2, …
 *   3. Records the (syno_disk_name → target_idx) map and calls
 *      scsi_force_replug() on each disk so sd_probe() re-runs.
 *   4. At SCSI_EVT_DEV_PROBING for each replug the pending table is set.
 *   5. Our ida_alloc_range() override forces the exact target index so the
 *      disk is renamed to the desired sd-letter (sda, sdb, …).
 *
 * Optional explicit override: write "sdc:0,sdi:1" to the same proc file.
 *
 * KERNEL COMPATIBILITY
 * ida_alloc_range() exists on kernels >= 4.19.  On older kernels the IDA
 * override is a no-op passthrough (remapping silently disabled).
 */

#include "diskidxmap_shim.h"
#include "../shim_base.h"
#include "../../common.h"
#include <linux/version.h>
#include "../../internal/scsi/scsi_notifier.h"
#include "../../internal/scsi/scsi_toolbox.h"
#include "../../internal/call_protected.h"
#include "../../internal/override/override_symbol.h"
#include "../boot_dev/boot_shim_base.h"
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/sort.h>
#include <linux/slab.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#define SHIM_NAME "diskidxmap"
#define PROC_NAME "diskidxmap"
#define DISKIDXMAP_MAX 32

/* ------------------------------------------------------------------ */
/*  Map table (syno_disk_name → target_idx)                            */
/* ------------------------------------------------------------------ */

struct diskidxmap_entry {
    char name[16];
    unsigned int target_idx;
};

static struct diskidxmap_entry map_table[DISKIDXMAP_MAX];
static int map_count = 0;
static DEFINE_SPINLOCK(map_lock);

/* ------------------------------------------------------------------ */
/*  Pending-probe table: sdp → target_idx, active during replug probe  */
/* ------------------------------------------------------------------ */

struct pending_entry {
    struct scsi_device *sdp;
    unsigned int target_idx;
};
static struct pending_entry pending[DISKIDXMAP_MAX];
static DEFINE_SPINLOCK(pending_lock);

static override_symbol_inst *ida_ovs = NULL;

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static int find_in_map(const char *name, unsigned int *out_idx)
{
    unsigned long flags;
    int i, found = 0;
    spin_lock_irqsave(&map_lock, flags);
    for (i = 0; i < map_count; i++) {
        if (strcmp(map_table[i].name, name) == 0) {
            *out_idx = map_table[i].target_idx;
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&map_lock, flags);
    return found;
}

static bool is_boot_disk(struct scsi_device *sdp)
{
    void *shimmed = get_shimmed_boot_dev();
    return shimmed && (sdp == shimmed);
}

static bool get_pci_bdf(struct scsi_device *sdp,
                         u32 *domain, u8 *bus, u8 *devno, u8 *fn)
{
    struct device *dev = sdp->host->shost_gendev.parent;
    while (dev) {
        if (dev->bus && strcmp(dev->bus->name, "pci") == 0) {
            struct pci_dev *pdev = to_pci_dev(dev);
            *domain = pci_domain_nr(pdev->bus);
            *bus    = pdev->bus->number;
            *devno  = PCI_SLOT(pdev->devfn);
            *fn     = PCI_FUNC(pdev->devfn);
            return true;
        }
        dev = dev->parent;
    }
    *domain = 0xffffffff; *bus = 0xff; *devno = 0xff; *fn = 0xff;
    return false;
}

/* ------------------------------------------------------------------ */
/*  ida_alloc_range() override                                          */
/* ------------------------------------------------------------------ */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
static int ida_alloc_range_trap(struct ida *ida, unsigned int min,
                                 unsigned int max, gfp_t gfp_mask)
{
    unsigned long flags;
    unsigned int target = 0;
    bool found = false;
    int i;

    spin_lock_irqsave(&pending_lock, flags);
    for (i = 0; i < DISKIDXMAP_MAX; i++) {
        if (pending[i].sdp) {
            target = pending[i].target_idx;
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&pending_lock, flags);

    if (!found)
        return _ida_alloc_range(ida, min, max, gfp_mask);

    pr_loc_dbg("diskidxmap: ida_alloc_range forcing index %u (was min=%u max=%u)",
               target, min, max);
    return _ida_alloc_range(ida, target, target, gfp_mask);
}
#endif

static int install_ida_override(void)
{
    if (ida_ovs)
        return 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
    ida_ovs = override_symbol("ida_alloc_range", ida_alloc_range_trap);
    if (IS_ERR_OR_NULL(ida_ovs)) {
        pr_loc_err("diskidxmap: failed to override ida_alloc_range - error=%ld",
                   PTR_ERR(ida_ovs));
        ida_ovs = NULL;
        return -EFAULT;
    }
#else
    pr_loc_wrn("diskidxmap: kernel < 4.19, IDA override unavailable — remapping disabled");
#endif
    return 0;
}

static void remove_ida_override(void)
{
    if (!ida_ovs)
        return;
    restore_symbol(ida_ovs);
    ida_ovs = NULL;
}

/* ------------------------------------------------------------------ */
/*  SCSI probe event handler                                            */
/* ------------------------------------------------------------------ */

static int scsi_disk_probe_handler(struct notifier_block *self,
                                    unsigned long state, void *data)
{
    struct scsi_device *sdp = data;
    unsigned int target_idx;
    unsigned long flags;
    int i;

    if (state == SCSI_EVT_DEV_PROBING) {
        if (!sdp->syno_disk_name[0])
            return NOTIFY_DONE;
        if (!find_in_map(sdp->syno_disk_name, &target_idx))
            return NOTIFY_DONE;

        pr_loc_dbg("diskidxmap: PROBING %s → target index %u",
                   sdp->syno_disk_name, target_idx);

        spin_lock_irqsave(&pending_lock, flags);
        for (i = 0; i < DISKIDXMAP_MAX; i++) {
            if (!pending[i].sdp) {
                pending[i].sdp = sdp;
                pending[i].target_idx = target_idx;
                break;
            }
        }
        spin_unlock_irqrestore(&pending_lock, flags);
        return NOTIFY_OK;
    }

    if (state == SCSI_EVT_DEV_PROBED_OK || state == SCSI_EVT_DEV_PROBED_ERR) {
        spin_lock_irqsave(&pending_lock, flags);
        for (i = 0; i < DISKIDXMAP_MAX; i++) {
            if (pending[i].sdp == sdp) {
                pending[i].sdp = NULL;
                pending[i].target_idx = 0;
                break;
            }
        }
        spin_unlock_irqrestore(&pending_lock, flags);
        return NOTIFY_OK;
    }

    return NOTIFY_DONE;
}

static struct notifier_block scsi_disk_nb = {
    .notifier_call = scsi_disk_probe_handler,
    .priority = INT_MIN + 1,
};

/* ------------------------------------------------------------------ */
/*  Replug walker: replug any disk that has a map entry                 */
/* ------------------------------------------------------------------ */

static int replug_mapped_disk(struct scsi_device *sdp)
{
    unsigned int idx;
    if (!sdp->syno_disk_name[0])
        return 0;
    if (!find_in_map(sdp->syno_disk_name, &idx))
        return 0;
    pr_loc_dbg("diskidxmap: replugging %s → %u", sdp->syno_disk_name, idx);
    scsi_force_replug(sdp);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Auto sort + assign: build map, then replug                          */
/* ------------------------------------------------------------------ */

struct disk_sort_entry {
    u32 domain;
    u8  bus, devno, fn;
    u32 host, channel, id;
    u64 lun;
    struct scsi_device *sdp;
};

static int disk_sort_cmp(const void *a, const void *b)
{
    const struct disk_sort_entry *x = a, *y = b;
#define CMP(f) if (x->f != y->f) return x->f < y->f ? -1 : 1
    CMP(domain); CMP(bus); CMP(devno); CMP(fn);
    CMP(host); CMP(channel); CMP(id); CMP(lun);
#undef CMP
    return 0;
}

struct collect_ctx {
    struct disk_sort_entry entries[DISKIDXMAP_MAX];
    int count;
};

static int collect_disk(struct scsi_device *sdp, void *ctx_ptr)
{
    struct collect_ctx *ctx = ctx_ptr;
    struct disk_sort_entry *e;

    if (ctx->count >= DISKIDXMAP_MAX)
        return 0;
    if (is_boot_disk(sdp))
        return 0;
    if (opportunistic_read_capacity(sdp) <= 0)
        return 0;

    e = &ctx->entries[ctx->count++];
    e->sdp     = sdp;
    e->host    = sdp->host->host_no;
    e->channel = sdp->channel;
    e->id      = sdp->id;
    e->lun     = sdp->lun;
    get_pci_bdf(sdp, &e->domain, &e->bus, &e->devno, &e->fn);
    return 0;
}

static void do_auto_remap(void)
{
    struct collect_ctx *ctx;
    unsigned long flags;
    int i, out;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) {
        pr_loc_err("diskidxmap: OOM in auto_remap");
        return;
    }

    out = for_each_scsi_disk_ctx(collect_disk, ctx);
    if (out != 0 && out != -ENXIO) {
        pr_loc_wrn("diskidxmap: for_each_scsi_disk_ctx error=%d", out);
        kfree(ctx);
        return;
    }

    if (ctx->count == 0) {
        pr_loc_dbg("diskidxmap: auto_remap: no eligible disks found");
        kfree(ctx);
        return;
    }

    sort(ctx->entries, ctx->count, sizeof(ctx->entries[0]), disk_sort_cmp, NULL);

    /* Build the name→idx map. */
    spin_lock_irqsave(&map_lock, flags);
    map_count = 0;
    for (i = 0; i < ctx->count; i++) {
        struct scsi_device *sdp = ctx->entries[i].sdp;
        if (!sdp->syno_disk_name[0])
            continue;
        strscpy(map_table[map_count].name, sdp->syno_disk_name,
                sizeof(map_table[map_count].name));
        map_table[map_count].target_idx = (unsigned int)map_count;
        pr_loc_inf("diskidxmap: auto %s → index %u",
                   map_table[map_count].name, map_table[map_count].target_idx);
        map_count++;
    }
    spin_unlock_irqrestore(&map_lock, flags);

    /* Force-replug each disk so it re-probes with the new index. */
    for (i = 0; i < ctx->count; i++) {
        struct scsi_device *sdp = ctx->entries[i].sdp;
        if (sdp->syno_disk_name[0])
            scsi_force_replug(sdp);
    }

    kfree(ctx);
}

/* ------------------------------------------------------------------ */
/*  Parse explicit "name:idx[,...]" string into map_table              */
/* ------------------------------------------------------------------ */

static int parse_explicit_map(const char *str)
{
    char *copy, *entry, *comma, *colon;
    unsigned long idx_val;
    unsigned long flags;
    int count = 0;

    copy = kstrdup(str, GFP_KERNEL);
    if (!copy)
        return -ENOMEM;

    spin_lock_irqsave(&map_lock, flags);
    map_count = 0;
    entry = copy;
    while (entry && *entry && map_count < DISKIDXMAP_MAX) {
        comma = strchr(entry, ',');
        if (comma) *comma = '\0';

        colon = strchr(entry, ':');
        if (!colon) goto next;
        *colon = '\0';

        if (kstrtoul(colon + 1, 10, &idx_val) != 0) goto next;

        strscpy(map_table[map_count].name, entry,
                sizeof(map_table[map_count].name));
        map_table[map_count].target_idx = (unsigned int)idx_val;
        pr_loc_inf("diskidxmap: explicit %s → index %u",
                   map_table[map_count].name, map_table[map_count].target_idx);
        map_count++;
        count++;
    next:
        entry = comma ? comma + 1 : NULL;
    }
    spin_unlock_irqrestore(&map_lock, flags);

    kfree(copy);
    return count;
}

/* ------------------------------------------------------------------ */
/*  /proc/sys/kernel/diskidxmap                                         */
/* ------------------------------------------------------------------ */

static ssize_t proc_write(struct file *file, const char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    char *kbuf;
    ssize_t ret = (ssize_t)count;

    if (count == 0 || count > 512)
        return -EINVAL;

    kbuf = kmalloc(count + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, ubuf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[count] = '\0';

    /* Strip trailing whitespace. */
    {
        size_t l = strlen(kbuf);
        while (l > 0 && (kbuf[l-1] == '\n' || kbuf[l-1] == '\r' || kbuf[l-1] == ' '))
            kbuf[--l] = '\0';
    }

    if (strcmp(kbuf, "auto") == 0) {
        pr_loc_inf("diskidxmap: 'auto' triggered");
        do_auto_remap();
    } else {
        int n = parse_explicit_map(kbuf);
        if (n < 0) {
            ret = n;
        } else {
            pr_loc_inf("diskidxmap: explicit map (%d entries), triggering replug", n);
            int out = for_each_scsi_disk(replug_mapped_disk);
            if (out != 0 && out != -ENXIO)
                pr_loc_wrn("diskidxmap: replug walker error=%d (non-fatal)", out);
        }
    }

    kfree(kbuf);
    return ret;
}

static ssize_t proc_read(struct file *file, char __user *ubuf,
                          size_t count, loff_t *ppos)
{
    char buf[512];
    int len = 0;
    unsigned long flags;
    int i;

    spin_lock_irqsave(&map_lock, flags);
    for (i = 0; i < map_count && len < (int)sizeof(buf) - 32; i++)
        len += scnprintf(buf + len, sizeof(buf) - len,
                         "%s:%u\n", map_table[i].name, map_table[i].target_idx);
    spin_unlock_irqrestore(&map_lock, flags);

    if (len == 0)
        len = scnprintf(buf, sizeof(buf), "(empty)\n");

    return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops diskidxmap_fops = {
    .proc_read  = proc_read,
    .proc_write = proc_write,
};
#else
static const struct file_operations diskidxmap_fops = {
    .owner = THIS_MODULE,
    .read  = proc_read,
    .write = proc_write,
};
#endif

static struct proc_dir_entry *proc_entry = NULL;

/* ------------------------------------------------------------------ */
/*  Register / unregister                                               */
/* ------------------------------------------------------------------ */

int register_diskidxmap_shim(void)
{
    int out;

    shim_reg_in();

    memset(map_table, 0, sizeof(map_table));
    memset(pending,   0, sizeof(pending));

    out = install_ida_override();
    if (out != 0)
        return out;

    out = subscribe_scsi_disk_events(&scsi_disk_nb);
    if (out != 0) {
        pr_loc_err("diskidxmap: failed to subscribe SCSI events - error=%d", out);
        remove_ida_override();
        return out;
    }

    /* Create /proc/diskidxmap (always exists under /proc directly). */
    proc_entry = proc_create(PROC_NAME, 0644, NULL, &diskidxmap_fops);
    if (!proc_entry) {
        pr_loc_err("diskidxmap: failed to create /proc/%s", PROC_NAME);
        unsubscribe_scsi_disk_events(&scsi_disk_nb);
        remove_ida_override();
        return -ENOMEM;
    }

    pr_loc_inf("diskidxmap: ready — write 'auto' or 'name:idx,...' to /proc/%s",
               PROC_NAME);
    shim_reg_ok();
    return 0;
}

int unregister_diskidxmap_shim(void)
{
    shim_ureg_in();

    if (proc_entry) {
        remove_proc_entry(PROC_NAME, NULL);
        proc_entry = NULL;
    }
    unsubscribe_scsi_disk_events(&scsi_disk_nb);
    remove_ida_override();

    shim_ureg_ok();
    return 0;
}
