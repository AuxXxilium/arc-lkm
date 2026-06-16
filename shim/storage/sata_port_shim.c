/**
 * Allows for usage of SCSI-based storage devices like they were bare standard SATA ones
 *
 * WHY THIS SHIM?
 * Normally Linux doesn't care if something is an SCSI device or a SATA one, as SATA is a subset of SCSI (technically
 * speaking SATA is an interface using SCSI protocol). However, the syno-modified SCSI driver (drivers/scsi/sd.c) adds
 * a layer of logical disk types. These types determine what the disk actually is, so that the NAS can know what should
 * be done with them.
 * For example SYNO_DISK_USB, SYNO_DISK_SYNOBOOT, SYNO_DISK_SATA, and SYNO_DISK_ISCSI are all normally visible in the
 * system as /dev/sdX and are all SCSI-based drives. However, you can only use RAID on SATA drives and not on USB ones.
 * The "SYNO_DISK_SATA" is kind-of a catch-all type for all disks which are used for storing data, even if they're not
 * really SATA disks. One of the exceptions set by the sd.c driver is that if VirtIO driver is used all disks connected
 * via that method are treated as SYNO_DISK_SATA. Unfortunately that, very logical and useful, assumption is made ONLY
 * when the kernel is compiled with CONFIG_SYNO_KVMX64 (which is a special platform for VDSM). On all other platforms
 * disks connected to VirtIO will be slightly broken in old versions and unusable in newer ones (as their tpe is set to
 * SYNO_DISK_UNKNOWN). This shim brings the functionality available on CONFIG_SYNO_KVMX64 to all platforms.
 * In addition, it changes SAS ports to be SATA as well as syno reserves SYNO_DISK_SAS for usage with just a few FS
 * devices and external enclosures.
 *
 * HOW DOES IT WORK?
 * It simply plugs into the SCSI driver (via SCSI notifier) and waits for a new drive. When a new drive is connected it
 * checks if it was connected via the VirtIO driver or through a SAS card driver and changes the port type to
 * SYNO_PORT_TYPE_SATA, which will later force the driver to assume the drive is indeed a "SATA" drive (SYNO_DISK_SATA).
 * While the ports can be enumerated and changed all at once, it's safer to do it per-drive basis as drivers allow for
 * ports to be dynamically reconfigured and thus the type may change. This is also why we make no effort of
 * restoring port types after this shim is unregistered.
 *
 * References
 *   - drivers/scsi/sd.c in Linux sources
 */
#include "sata_port_shim.h"
#include "../shim_base.h"
#include "../../common.h"
#include "../../internal/scsi/scsi_toolbox.h" //scsi_force_replug()
#include "../../internal/scsi/scsi_notifier.h"
#include "../../internal/helper/symbol_helper.h" //kln_func
#include <scsi/scsi_device.h> //struct scsi_device
#include <scsi/scsi_host.h> //struct Scsi_Host, SYNO_PORT_TYPE_*
#include <linux/pci.h> //struct pci_dev, to_pci_dev()
#include "../../config/runtime_config.h"
#include "../../config/platform_types.h"

#define SHIM_NAME "SATA port emulator"
#define VIRTIO_HOST_ID "Virtio SCSI HBA"

/* Some kernels/platform headers do not provide struct scsi_device::syno_block_info. */
#ifdef BLOCK_INFO_SIZE
#define RP_HAS_SYNO_BLOCK_INFO 1
#else
#define RP_HAS_SYNO_BLOCK_INFO 0
#endif

typedef int (*syno_pciepath_dts_pattern_get_t)(struct pci_dev *pdev, char *buf, int buf_len);

static bool host_uses_libata(const struct Scsi_Host *host);
#if RP_HAS_SYNO_BLOCK_INFO
static const char *resolve_syno_pciepath(struct device *pci_dev, char *out_buf, size_t out_len);
#endif

#if RP_HAS_SYNO_BLOCK_INFO
static bool resolve_hba_port_no(const struct scsi_device *sdp, unsigned int *out_port_no)
{
    struct device *dev;
    unsigned int host_no;
    unsigned int port_no;

    if (unlikely(!sdp || !out_port_no))
        return false;

    /*
     * For mptsas/mpt3sas topology, parents include nodes like "port-30:4".
     * The second number is the per-controller port index DSM DT mapping expects.
     */
    dev = (struct device *)&sdp->sdev_gendev;
    while (dev) {
        const char *name = dev_name(dev);

        if (name && sscanf(name, "port-%u:%u", &host_no, &port_no) == 2) {
            *out_port_no = port_no;
            return true;
        }

        dev = dev->parent;
    }

    return false;
}
#endif

static bool host_has_pci_parent(const struct Scsi_Host *host)
{
    struct device *dev;

    if (unlikely(!host))
        return false;

    dev = host->shost_gendev.parent;
    while (dev) {
        if (dev->bus && strcmp(dev->bus->name, "pci") == 0)
            return true;

        dev = dev->parent;
    }

    return false;
}

/*
 * Returns true only when the PCI parent of this SCSI host is a storage controller
 * (PCI class 0x01xxxx).  Network adapters like mlx5 (class 0x02xxxx) also expose a
 * SCSI host on AMD/DT platforms and must not be treated as data-disk controllers.
 */
static bool host_pci_parent_is_storage(const struct Scsi_Host *host)
{
    struct device *dev;

    if (unlikely(!host))
        return false;

    dev = host->shost_gendev.parent;
    while (dev) {
        if (dev->bus && strcmp(dev->bus->name, "pci") == 0) {
            struct pci_dev *pdev = to_pci_dev(dev);
            /* PCI class is stored as (class << 8 | revision) in pci_dev->class.
             * The top byte is the base class; 0x01 == Mass Storage Controller. */
            return ((pdev->class >> 16) & 0xff) == 0x01;
        }
        dev = dev->parent;
    }

    return false;
}

#if RP_HAS_SYNO_BLOCK_INFO
static const char *resolve_syno_pciepath(struct device *pci_dev, char *out_buf, size_t out_len)
{
    static syno_pciepath_dts_pattern_get_t syno_get_path = NULL;
    static bool resolver_checked = false;

    if (unlikely(!pci_dev))
        return "";

    if (!resolver_checked) {
        unsigned long addr = kln_func ? kln_func("syno_pciepath_dts_pattern_get") : 0;

        if (addr)
            syno_get_path = (syno_pciepath_dts_pattern_get_t)addr;

        resolver_checked = true;
    }

    if (syno_get_path && out_buf && out_len > 0) {
        out_buf[0] = '\0';
        if (syno_get_path(to_pci_dev(pci_dev), out_buf, (int)out_len) == 0 && out_buf[0] != '\0')
            return out_buf;
    }

    return dev_name(pci_dev);
}
#endif

/**
 * Populates sdp->syno_block_info for HBA-backed disks in DT mode.
 *
 * Root cause: Synology's sd.c only fills syno_block_info during sd_probe()
 * for libata/ahci-backed disks (reading from the ata_port structure).
 * For HBA drivers like mptsas the libata code path never runs, so the field
 * stays empty.  DSM's DT disks.sh reads syno_block_info (pciepath +
 * ata_port_no + driver) to assign a physical slot; without it the disk is
 * invisible in Storage Manager even though the block device exists.
 *
 * We derive the values at shim time by walking the device-parent chain to
 * the PCI device (dev_name() already returns the BDF string) and using the
 * SCSI target-id as the port/slot index — which matches the ahci ata_port_no
 * convention that disks.sh expects.
 *
 * Note: include <linux/pci.h> so we can call Synology's DTS pciepath helper
 * when available, falling back to dev_name() BDF format otherwise.
 */
#if RP_HAS_SYNO_BLOCK_INFO
static void populate_syno_block_info_if_needed(struct scsi_device *sdp)
{
    struct device *dev;
    const char *driver_name;
    const char *syno_driver_name;
    const char *pciepath;
    unsigned int ata_port_no;
    char dts_pciepath[128];

    if (!current_config.hw_config || !current_config.hw_config->is_dt)
        return;

    /* Only non-libata DT disks need this; libata/ahci hosts already populate it via sd_probe() */
    if (host_uses_libata(sdp->host) || !host_has_pci_parent(sdp->host))
        return;

    /*
     * Never overwrite syno_block_info that the driver (e.g. ahci/libata via sd.c) already
     * filled in. Without this guard, SATA controllers whose hostt->name isn't in the
     * host_uses_libata allowlist would have their driver-set syno_block_info replaced with
     * our approximate values, causing wrong slot assignments and invisible disks in SM.
     */
    if (sdp->syno_block_info[0] != '\0')
        return;

    /* Walk up the device-parent chain until we land on a PCI device */
    dev = sdp->host->shost_gendev.parent;
    while (dev) {
        if (dev->bus && strcmp(dev->bus->name, "pci") == 0)
            break;
        dev = dev->parent;
    }

    if (unlikely(!dev)) {
        pr_loc_wrn("No PCI parent found for HBA host%d - syno_block_info stays empty",
                   sdp->host->host_no);
        return;
    }

    driver_name = sdp->host->hostt->proc_name;
    if (!driver_name || !*driver_name)
        driver_name = sdp->host->hostt->name;

    /*
     * DT userspace slot mapping is conservative about storage driver names.
     * For synthetic HBA entries, present the same logical class as onboard SATA
     * so the disk is accepted by Storage Manager while keeping accurate
     * pciepath and ata_port_no values.
     */
    syno_driver_name = "ahci";

    /*
     * Prefer explicit HBA port index from topology node name (port-H:P).
     * Fall back to SCSI target ID if topology does not expose a port node.
     */
    ata_port_no = sdp->id;
    if (!resolve_hba_port_no(sdp, &ata_port_no))
        pr_loc_dbg("No port-H:P node for /dev/%s; falling back to sdp->id=%u",
                   sdp->syno_disk_name, sdp->id);

    pciepath = resolve_syno_pciepath(dev, dts_pciepath, sizeof(dts_pciepath));

    /*
     * Prefer Synology DTS pciepath format when helper symbol is available.
     * Fall back to PCI BDF (dev_name) to keep compatibility on kernels without that symbol.
     * Keep the same newline-delimited format used by native ahci/libata paths
     * so DSM userspace parsers handle these entries consistently.
     */
    if (snprintf(sdp->syno_block_info, 128,
                 "pciepath=%s\nata_port_no=%u\ndriver=%s\n",
                 pciepath, ata_port_no, syno_driver_name) > 0)
        pr_loc_dbg("Populated syno_block_info for /dev/%s: pciepath=%s ata_port_no=%u driver=%s (host driver=%s)",
                   sdp->syno_disk_name, pciepath, ata_port_no,
                   syno_driver_name, driver_name ? driver_name : "scsi");
    else
        pr_loc_wrn("snprintf failed for syno_block_info /dev/%s", sdp->syno_disk_name);
}
#else
static void populate_syno_block_info_if_needed(struct scsi_device *sdp)
{
    (void)sdp;
}
#endif

static bool host_uses_libata(const struct Scsi_Host *host)
{
    if (unlikely(!host || !host->hostt || !host->hostt->name))
        return false;

    const char *name = host->hostt->name;

    return strcmp(name, "ahci") == 0 ||
           strcmp(name, "ahci_platform") == 0 ||
           strcmp(name, "ata_piix") == 0 ||
           strcmp(name, "libata") == 0 ||
           strncmp(name, "sata_", 5) == 0 ||
           strncmp(name, "pata_", 5) == 0;
}

/**
 * Checks if we should fix a given device or ignore it
 */
static bool is_fixable(struct scsi_device *sdp)
{
    if (unlikely(!sdp || !sdp->host || !sdp->host->hostt))
        return false;

    const char *host_name = sdp->host->hostt->name;
    const bool uses_libata = host_uses_libata(sdp->host);
    const bool non_sata_port = sdp->host->hostt->syno_port_type != SYNO_PORT_TYPE_SATA;
    const bool is_sas_port = sdp->host->hostt->syno_port_type == SYNO_PORT_TYPE_SAS;
    const char *proc_name = sdp->host->hostt->proc_name;
    const bool is_virtio_host = (host_name && strcmp(host_name, VIRTIO_HOST_ID) == 0) ||
                                (proc_name && strcmp(proc_name, "virtio_scsi") == 0);
    const bool is_pci_attached_scsi = !uses_libata && host_pci_parent_is_storage(sdp->host);

    // Non-DT models: fix SAS-type ports and VirtIO SCSI only.  This matches 25.11.26 behaviour.
    // is_pci_attached_scsi is intentionally excluded here: on non-DT, DSM uses the internalportcfg
    // bitmask (set by nondtModel in disks.sh) to identify data disks regardless of syno_disk_type,
    // so HBA drivers that leave syno_port_type==0 (e.g. VMware mptsas) do not need the type fixed.
    // Including is_pci_attached_scsi caused Synology's sd.c to enter its SATA port-index retry loop
    // (want_idx) for every disk on the HBA, stalling each probe by ~15 seconds.
    if (likely(current_config.hw_config && !current_config.hw_config->is_dt)) {
        return (is_sas_port || (non_sata_port && is_virtio_host));
    }

    // DT safety mode: do not touch libata/native SATA; allow old SAS/VirtIO behavior plus generic PCI SCSI hosts.
    if (uses_libata || !(is_sas_port || is_virtio_host || is_pci_attached_scsi))
        return false;

    return true;
}

/**
 * Processes any new devices connected to the system AND existing devices which were forcefully reconnected
 *
 * When a device which is deemed fixable it will replace its port to SATA to make it work as a standard SATA drive.
 *
 * @return 0 on success, -E on error
 */
static int on_new_scsi_disk_device(struct scsi_device *sdp)
{
    if (!is_fixable(sdp))
        return 0;

    // For DT models, HBA-backed PCI storage disks must NOT have their port type changed here.
    // This hook fires at SCSI_EVT_DEV_PROBING, before sd_probe() runs.  Changing syno_port_type
    // to SATA for HBA disks at this point causes Synology's sd.c to enter its SATA port-index
    // assignment retry loop (want_idx) which stalls each probe by ~15 seconds.
    // populate_syno_block_info_if_needed() (the reason those DT disks appear in is_fixable()) runs
    // at PROBED_OK and does not require the port type to be changed beforehand.
    // SAS and VirtIO hosts on DT platforms (e.g. AMD + fake-SATA loader) still need the port type
    // fixed at PROBING so that sd.c assigns the correct syno_disk_type during sd_probe().
    if (current_config.hw_config && current_config.hw_config->is_dt) {
        const bool is_pci_storage_scsi = host_pci_parent_is_storage(sdp->host) &&
                                         !host_uses_libata(sdp->host);
        if (is_pci_storage_scsi)
            return 0;
    }

    pr_loc_dbg("Found new disk vendor=\"%s\" model=\"%s\" connected to \"%s\" HBA over non-SATA port (type=%d) - "
               "fixing to SATA port (type=%d)", sdp->vendor, sdp->model, sdp->host->hostt->name,
               sdp->host->hostt->syno_port_type, SYNO_PORT_TYPE_SATA);

    sdp->host->hostt->syno_port_type = SYNO_PORT_TYPE_SATA;

    return 0;
}

/**
 * Called for every existing SCSI-based disk to determine if there are any fixable devices which are already connected
 *
 * For non-DT platforms every fixable device that is still connected with a non-SATA port type is forcefully
 * re-connected, as that is the only way to re-run sd_probe() with the corrected port type.  An in-place
 * syno_port_type fix is insufficient because sd_probe() already ran and set the disk's syno_disk_type based on the
 * original (wrong) port type.  Without re-probing, DSM still sees a non-SATA disk and refuses to format it.
 *
 * For DT platforms we fix the port type in-place without replug.  The DT is_fixable() path was added solely to
 * support populate_syno_block_info_if_needed() firing via SCSI_EVT_DEV_PROBED_OK; force-replugging DT HBA disks
 * was never tested and could interfere with sysfs slot mapping.
 *
 * @return 0 on success, -E on error
 */
static int on_existing_scsi_disk_device(struct scsi_device *sdp)
{
    if (!is_fixable(sdp))
        return 0;

    // DT + HBA PCI storage disk: fix the port type in-place, no replug.
    // Force-replugging DT HBA disks was never tested and could interfere with sysfs slot mapping;
    // populate_syno_block_info_if_needed() runs at PROBED_OK and handles slot assignment.
    // SAS and VirtIO disks on DT platforms follow the non-DT replug path below so that sd.c
    // re-runs sd_probe() with the corrected port type and assigns the right syno_disk_type.
    if (current_config.hw_config && current_config.hw_config->is_dt) {
        const bool is_pci_storage_scsi = host_pci_parent_is_storage(sdp->host) &&
                                         !host_uses_libata(sdp->host);
        if (is_pci_storage_scsi) {
            pr_loc_dbg(
                    "DT: fixing port type in-place for existing disk vendor=\"%s\" model=\"%s\" on \"%s\" HBA"
                    " (type %d -> %d, no replug).",
                    sdp->vendor, sdp->model, sdp->host->hostt->name,
                    sdp->host->hostt->syno_port_type, SYNO_PORT_TYPE_SATA);
            sdp->host->hostt->syno_port_type = SYNO_PORT_TYPE_SATA;
            return 0;
        }
    }

    pr_loc_dbg(
            "Found initialized disk vendor=\"%s\" model=\"%s\" connected to \"%s\" HBA over non-SATA port (type=%d)."
            " It must be auto-replugged to fix it.", sdp->vendor, sdp->model, sdp->host->hostt->name,
            sdp->host->hostt->syno_port_type);

    //After that it will land in on_new_scsi_disk_device()
    scsi_force_replug(sdp);

    return 0;
}

/**
 * Tiny shim to direct SCSI notifications to on_existing_scsi_disk_device() before it's probed
 */
static int scsi_disk_probe_handler(struct notifier_block *self, unsigned long state, void *data)
{
    if (state == SCSI_EVT_DEV_PROBING) {
        on_new_scsi_disk_device(data);
        return NOTIFY_OK;
    }

    /*
     * SCSI_EVT_DEV_PROBED_OK fires after sd_probe() returns successfully.
     * By this point sd.c has set syno_disk_name and populated syno_block_info
     * for libata/ahci disks.  For HBA disks (mptsas, mpt3sas, etc.) sd.c
     * leaves syno_block_info empty; we fill it here so disks.sh DT slot
     * mapping can assign these disks to physical slots.
     */
    if (state == SCSI_EVT_DEV_PROBED_OK) {
        populate_syno_block_info_if_needed(data);
        return NOTIFY_OK;
    }

    return NOTIFY_DONE;
}

static struct notifier_block scsi_disk_nb = {
    .notifier_call = scsi_disk_probe_handler,
    .priority = INT_MIN, //run late so prior notifiers can observe/adjust original values first
};

int register_sata_port_shim(void)
{
    shim_reg_in();

    int out;

    pr_loc_dbg("Registering for new devices notifications");
    out = subscribe_scsi_disk_events(&scsi_disk_nb);
    if (unlikely(out != 0)) {
        pr_loc_err("Failed to register for SCSI disks notifications - error=%d", out);
        return out;
    }

    pr_loc_dbg("Iterating over existing devices");
    out = for_each_scsi_disk(on_existing_scsi_disk_device);
    if (unlikely(out != 0 && out != -ENXIO)) {
        pr_loc_err("Failed to enumerate current SCSI disks - error=%d", out);
        return out;
    }
    
    shim_reg_ok();
    return 0;
}

int unregister_sata_port_shim(void)
{
    shim_ureg_in();

    unsubscribe_scsi_disk_events(&scsi_disk_nb);

    shim_ureg_ok();
    return 0; //noop
}