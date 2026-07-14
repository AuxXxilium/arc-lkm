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
#include <linux/ctype.h> //isprint()
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

/*
 * Returns true only when the PCI parent of this SCSI host is a storage controller
 * (PCI class 0x01xxxx).  Network adapters like mlx5 (class 0x02xxxx) and USB host
 * controllers (class 0x0Cxxxx) also expose a SCSI host on some platforms and must
 * not be treated as data-disk controllers.
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
 * Sanitizes sdp->rev (SCSI INQUIRY firmware revision, bytes 32-35) in place, once, right after
 * sd_probe() populates it.
 *
 * Root cause: some HBAs/SATL bridges return a truncated or otherwise malformed INQUIRY response
 * while still reporting a nominal transfer length to the mid-layer, so sdp->rev can end up with
 * non-space garbage past the real version string (e.g. "2.0" followed by undefined bytes). The
 * kernel mid-layer copies that raw field into sdp->rev verbatim and never touches it again -
 * every consumer (sysfs device/rev + device/firmware_rev, DSM's synostoraged which writes
 * /run/synostorage/disks/$dev/firm, and any ioctl path) reads the same live struct field, so
 * sanitizing it here once at probe time fixes all of them at the source instead of scrubbing
 * copies in each individual reader. This previously showed up as e.g. "2.0 ????" in Storage
 * Manager and in /run/synostorage/disks/$dev/firm.
 *
 * Unlike populate_syno_block_info_if_needed() this is NOT DT-only or HBA-only - a short/malformed
 * INQUIRY response is a property of the specific controller/SATL bridge, not of DT vs non-DT or
 * libata vs HBA, so this runs for every disk.
 */
static void sanitize_disk_rev_field(struct scsi_device *sdp)
{
    /* sdp->rev is declared "const unsigned char rev[4]" in this kernel's struct scsi_device -
     * it's an API contract (sd_probe() sets it once and nothing else is meant to write it after),
     * not a hardware-enforced read-only mapping, so casting away constness to fix up the raw
     * INQUIRY bytes in place is safe here. */
    unsigned char *rev = (unsigned char *)sdp->rev;
    size_t i;

    for (i = 0; i < sizeof(sdp->rev); i++) {
        if (!isprint(rev[i]))
            rev[i] = ' ';
    }
}

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

    /* Only non-libata DT disks on PCI storage controllers need this.
     * host_pci_parent_is_storage() (class 0x01) excludes USB host controllers (class 0x0C)
     * which would otherwise also pass host_has_pci_parent() and get a fake ahci syno_block_info. */
    if (host_uses_libata(sdp->host) || !host_pci_parent_is_storage(sdp->host))
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
    return scsi_host_uses_libata(host);
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

    // Non-DT models: fix SAS-type ports and VirtIO SCSI only.
    // is_pci_attached_scsi is intentionally excluded: on non-DT, DSM uses the internalportcfg
    // bitmask (set by nondtModel in disks.sh) to identify data disks regardless of syno_disk_type,
    // so HBA drivers that leave syno_port_type==0 do not need the type fixed.
    if (likely(current_config.hw_config && !current_config.hw_config->is_dt))
        return (is_sas_port || (non_sata_port && is_virtio_host));

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

    // DT-mode PCI-storage HBA disks (is_pci_storage_scsi) are fixed here too, same as SAS/VirtIO.
    // This means their syno_port_type is SATA before sd_probe() runs, so sd.c assigns them sataN
    // naming/slot mapping uniformly with native SATA disks instead of falling back to sdX/sasN.
    // Trade-off: this can drive sd.c into its SATA port-index assignment retry loop (want_idx),
    // stalling each affected disk's probe by ~15 seconds - accepted for naming consistency.
    pr_loc_dbg("Found new disk vendor=\"%s\" model=\"%s\" connected to \"%s\" HBA over non-SATA port (type=%d) - "
               "fixing to SATA port (type=%d)", sdp->vendor, sdp->model, sdp->host->hostt->name,
               sdp->host->hostt->syno_port_type, SYNO_PORT_TYPE_SATA);

    sdp->host->hostt->syno_port_type = SYNO_PORT_TYPE_SATA;

    return 0;
}

/**
 * Called for every existing SCSI-based disk to determine if there are any fixable devices which are already connected
 *
 * Every fixable device that is still connected with a non-SATA port type is forcefully re-connected, as that is the
 * only way to re-run sd_probe() with the corrected port type.  An in-place syno_port_type fix is insufficient
 * because sd_probe() already ran and set the disk's syno_disk_type based on the original (wrong) port type.
 * Without re-probing, DSM still sees a non-SATA disk and refuses to format it (non-DT) or names/maps it as
 * sdX/sasN instead of sataN (DT).
 *
 * @return 0 on success, -E on error
 */
static int on_existing_scsi_disk_device(struct scsi_device *sdp)
{
    if (!is_fixable(sdp))
        return 0;

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
     * By this point sd.c has set syno_disk_name, populated sdp->rev from the raw INQUIRY
     * response, and populated syno_block_info for libata/ahci disks. For HBA disks (mptsas,
     * mpt3sas, etc.) sd.c leaves syno_block_info empty; we fill it here so disks.sh DT slot
     * mapping can assign these disks to physical slots. sdp->rev is sanitized for every disk
     * regardless of DT/HBA status - see sanitize_disk_rev_field().
     */
    if (state == SCSI_EVT_DEV_PROBED_OK) {
        sanitize_disk_rev_field(data);
        populate_syno_block_info_if_needed(data);
        return NOTIFY_OK;
    }

    return NOTIFY_DONE;
}

static struct notifier_block scsi_disk_nb = {
    .notifier_call = scsi_disk_probe_handler,
    .priority = INT_MIN, //run late so prior notifiers can observe/adjust original values first
};

/**
 * for_each_scsi_disk() callback wrapper for sanitize_disk_rev_field()
 *
 * Needed because sanitize_disk_rev_field() returns void but on_scsi_device_cb expects an int.
 */
static int sanitize_disk_rev_field_cb(struct scsi_device *sdp)
{
    sanitize_disk_rev_field(sdp);
    return 0;
}

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

    /* Sanitize sdp->rev for disks that were already probed before this shim loaded (e.g. LKM
     * reload) - on_existing_scsi_disk_device() above only touches is_fixable() disks (SAS/VirtIO/
     * PCI-storage HBA needing a port-type fix), so an already-SATA-typed disk with a malformed
     * INQUIRY response would otherwise never get its rev field sanitized. */
    out = for_each_scsi_disk(sanitize_disk_rev_field_cb);
    if (unlikely(out != 0 && out != -ENXIO))
        pr_loc_wrn("Failed to sanitize rev field on existing SCSI disks - error=%d", out);

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