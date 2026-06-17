#include "scsi_toolbox.h"
#include "scsiparam.h" //SCSI_*
#include "../../common.h"
#include "../../internal/call_protected.h" //scsi_scan_host_selected()
#include <linux/dma-direction.h> //DMA_FROM_DEVICE
#include <linux/unaligned/be_byteshift.h> //get_unaligned_be32()
#include <linux/delay.h> //msleep
#include <scsi/scsi.h> //cmd consts (e.g. SERVICE_ACTION_IN), SCAN_WILD_CARD, and TYPE_DISK
#include <scsi/scsi_eh.h> //struct scsi_sense_hdr, scsi_sense_valid()
#include <scsi/scsi_host.h> //struct Scsi_Host, SYNO_PORT_TYPE_SATA
#include <linux/string.h> //strcmp, strncmp
#include <scsi/scsi_transport.h> //struct scsi_transport_template
#include <scsi/scsi_device.h> //struct scsi_device, scsi_execute_req(), scsi_is_sdev_device()

extern struct bus_type scsi_bus_type; //SCSI bus type for driver scanning

/**
 * Issues SCSI "READ CAPACITY (16)" command
 * Make sure you read what this function returns!
 *
 * @param sdp
 * @param buffer Pointer to a buffer of size SCSI_BUF_SIZE
 * @param sshdr Sense header
 * @return 0 on command success, >0 if command failed; if the command failed it MAY be repeated
 */
static int scsi_read_cap16(struct scsi_device *sdp, unsigned char *buffer, struct scsi_sense_hdr *sshdr)
{
    unsigned char cmd[16];
    memset(cmd, 0, 16);
    cmd[0] = SCSI_SERVICE_ACTION_IN_16;
    cmd[1] = SAI_READ_CAPACITY_16;
    cmd[13] = SCSI_RC16_LEN;
    memset(buffer, 0, SCSI_RC16_LEN);

    return scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE, buffer, SCSI_RC16_LEN, sshdr, SCSI_CMD_TIMEOUT,
                            SCSI_CMD_MAX_RETRIES, NULL);
}

/**
 * Issues SCSI "READ CAPACITY (10)" command
 * Make sure you read what this function returns!
 *
 * @param sdp
 * @param buffer Pointer to a buffer of size SCSI_BUF_SIZE
 * @param sshdr Sense header
 * @return 0 on command success, >0 if command failed; if the command failed it MAY be repeated
 */
static int scsi_read_cap10(struct scsi_device *sdp, unsigned char *buffer, struct scsi_sense_hdr *sshdr)
{
    unsigned char cmd[16];
    cmd[0] = READ_CAPACITY;
    memset(&cmd[1], 0, 9);
    memset(buffer, 0, 8);

    return scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE, buffer, 8, sshdr, SCSI_CMD_TIMEOUT, SCSI_CMD_MAX_RETRIES, NULL);
}

long long opportunistic_read_capacity(struct scsi_device *sdp)
{
    //some drives work only with the 16 version but older ones can only accept the older variant
    //to prevent false-positive "command failed" we need to try both
    bool use_cap16 = true;

    unsigned char *buffer = NULL;
    kmalloc_or_exit_int(buffer, SCSI_BUF_SIZE);

    int out;
    int sense_valid = 0;
    struct scsi_sense_hdr sshdr;
    int read_retry = SCSI_CAP_MAX_RETRIES;
    do {
        //It can return 0 or a positive integer; 0 means immediate success where 1 means an error. Depending on the error
        //the command may be repeated.
        out = (use_cap16) ? scsi_read_cap16(sdp, buffer, &sshdr) : scsi_read_cap10(sdp, buffer, &sshdr);
        if (out == 0)
            break; //command just succeeded

        if (unlikely(out > 0)) { //it's technically an error but we may be able to recover
            if (use_cap16) { //if we previously used CAP(16) and it failed we can try older CAP(10) [even on hard-fail]
                use_cap16 = false;
                continue;
            }

            //Some failures are hard-failure (e.g. drive doesn't support the cmd), some are soft-failures
            //In soft failures some are known to take more time (e.g. spinning rust is spinning up) and some should be
            //fast-repeat. We really only distinguish hard from soft and just wait some time for others
            //In a normal scenario this path will be cold as the drive will respond to CAP(16) or CAP(10) right away.

            sense_valid = scsi_sense_valid(&sshdr);
            if (!sense_valid) {
                pr_loc_dbg("Invalid sense - trying again");
                continue; //Sense invalid, this can be repeated right away
            }

            //Drive deliberately rejected the request and indicated that this situtation will not change
            if (sshdr.sense_key == ILLEGAL_REQUEST && (sshdr.asc == 0x20 || sshdr.asc == 0x24) && sshdr.ascq == 0x00) {
                pr_loc_err("Drive refused to provide capacity");
                kfree(buffer);
                return -EINVAL;
            }

            //Drive is busy - wait for some time
            if (sshdr.sense_key == UNIT_ATTENTION && sshdr.asc == 0x29 && sshdr.ascq == 0x00) {
                pr_loc_dbg("Drive busy during capacity pre-read (%d attempts left), trying again", read_retry-1);
                msleep(500); //if it's a spinning rust over USB we may need to wait
                continue;
            }
        }
    } while (--read_retry);

    if (out != 0) {
        pr_loc_err("Failed to pre-read capacity of the drive after %d attempts due to SCSI errors",
                   (SCSI_CAP_MAX_RETRIES - read_retry));
        kfree(buffer);
        return -EIO;
    }

    unsigned sector_size = get_unaligned_be32(&buffer[8]);
    unsigned long long lba = get_unaligned_be64(&buffer[0]);

    //Good up to 8192000000 pebibytes - good luck overflowing that :D
    long long size_mb = ((lba+1) * sector_size) / 1024 / 1024; //sectors * sector size = size in bytes

    kfree(buffer);
    return size_mb;
}

bool is_scsi_disk(struct scsi_device *sdp)
{
    return (likely(sdp) && (sdp)->type == TYPE_DISK);
}

bool scsi_host_uses_libata(const struct Scsi_Host *host)
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

bool is_sata_disk(struct device *dev)
{
    //from the kernel's pov SCSI devices include SCSI hosts, "leaf" devices, and others - this filters real SCSI devices
    if (!is_scsi_leaf(dev))
        return false;

    struct scsi_device *sdp = to_scsi_device(dev);

    //end/leaf devices can be disks or other things - filter only real disks
    //more than that use syno's private property (hey! not all of their kernel mods are bad ;)) to determine port which
    //a given device uses (vanilla kernel doesn't care about silly ports - SCSI is SCSI)
    if (!is_scsi_disk(sdp) || sdp->host->hostt->syno_port_type != SYNO_PORT_TYPE_SATA)
        return false;

    return true;
}

int scsi_force_replug(scsi_device *sdp)
{
    if (unlikely(!is_scsi_leaf(&sdp->sdev_gendev))) {
        pr_loc_bug("%s expected SCSI leaf - got something else", __FUNCTION__);
        return -EINVAL;
    }

    struct Scsi_Host *host = sdp->host;
    pr_loc_dbg("Removing device from host%d", host->host_no);
    scsi_remove_device(sdp); //this will do locking for remove

    //See drivers/scsi/scsi_sysfs.c:scsi_scan() for details
    if (unlikely(host->transportt->user_scan)) {
        pr_loc_dbg("Triggering template-based rescan of host%d", host->host_no);
        return host->transportt->user_scan(host, SCAN_WILD_CARD, SCAN_WILD_CARD, SCAN_WILD_CARD);
    } else {
        pr_loc_dbg("Triggering generic rescan of host%d", host->host_no);
        //this is unfortunately defined in scsi_scan.c, it can be emulated because it's just bunch of loops, but why?
        //This will also most likely never be used anyway
        return _scsi_scan_host_selected(host, SCAN_WILD_CARD, SCAN_WILD_CARD, SCAN_WILD_CARD, 1);
    }
}

//We assume that if the sd was loaded once it will never unload (as on most kernels it's built in).
//If this assumption changes the cache can simply be removed
bool sd_driver_loaded = false;
struct device_driver *find_scsi_driver(void)
{
    struct device_driver *drv = driver_find("sd", &scsi_bus_type);
    if (IS_ERR(drv)) {
        pr_loc_err("Failed to query sd driver status - error=%ld", PTR_ERR(drv));
        return drv;
    }

    if (drv) {
        sd_driver_loaded = true;
        return drv;
    }

    return NULL;
}

int is_scsi_driver_loaded(void)
{
    if (likely(sd_driver_loaded))
        return true;

    struct device_driver *drv = find_scsi_driver();
    if (IS_ERR(drv)) //get_scsi_driver() will already print an error message
        return PTR_ERR(drv);

    return drv ? SCSI_DRV_LOADED : SCSI_DRV_NOT_LOADED;
}

/**
 * Filters out all SCSI leafs and calls the callback prescribed
 */
static int for_each_scsi_leaf_filter(struct device *dev, on_scsi_device_cb cb)
{
    if (!is_scsi_leaf(dev))
        return 0;

    return (cb)(to_scsi_device(dev));
}

/**
 * Filters out all SCSI disks and calls the callback prescribed
 */
static int for_each_scsi_disk_filter(struct device *dev, on_scsi_device_cb cb)
{
    if (!is_scsi_leaf(dev))
        return 0;

    struct scsi_device *sdp = to_scsi_device(dev);
    if (!is_scsi_disk(sdp))
        return 0;

    return (cb)(to_scsi_device(dev));
}

static int inline for_each_scsi_x(on_scsi_device_cb *cb, int (*filter)(struct device *dev, on_scsi_device_cb cb))
{
    if (!is_scsi_driver_loaded())
        return -ENXIO;

    int code = bus_for_each_dev(&scsi_bus_type, NULL, cb, (int (*)(struct device *, void *))filter);
    return unlikely(code == -ENXIO) ? -EIO : code;
}

int for_each_scsi_leaf(on_scsi_device_cb *cb)
{
    return for_each_scsi_x(cb, for_each_scsi_leaf_filter);
}

int for_each_scsi_disk(on_scsi_device_cb *cb)
{
    return for_each_scsi_x(cb, for_each_scsi_disk_filter);
}

/*
 * Temperature log page (SPC-4, 7.3.13). The page header is 4 bytes, followed
 * by one or more log parameters. Parameter 0x0000 (Current Temperature) has
 * its value at byte offset 9 in the raw response.
 */
#define SCSI_LOG_PAGE_TEMP      0x0D
#define SCSI_LOG_TEMP_ALLOC     16
#define SCSI_LOG_TEMP_BYTE      9
#define SCSI_TEMP_UNAVAILABLE   0xFF

/**
 * Reads the current temperature of a SCSI device via LOG SENSE (Temperature page 0x0D)
 *
 * @param sdp SCSI device pointer
 * @return temperature in Celsius on success, -ENODATA if the drive reports it as unavailable,
 *         or -EIO if the LOG SENSE command itself failed
 */
int scsi_read_disk_temp(struct scsi_device *sdp)
{
    unsigned char cmd[10] = {0};
    unsigned char buf[SCSI_LOG_TEMP_ALLOC] = {0};
    struct scsi_sense_hdr sshdr;
    int ret;

    cmd[0] = LOG_SENSE;                              /* 0x4D */
    cmd[2] = 0x40 | SCSI_LOG_PAGE_TEMP;             /* PC=01 (cumulative values), page=0x0D */
    cmd[8] = SCSI_LOG_TEMP_ALLOC;                   /* allocation length */

    ret = scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE, buf, SCSI_LOG_TEMP_ALLOC, &sshdr,
                           SCSI_CMD_TIMEOUT, SCSI_CMD_MAX_RETRIES, NULL);
    if (ret != 0) {
        pr_loc_dbg("LOG SENSE temp page failed for /dev/%s (host%d) - err=%d",
                   sdp->syno_disk_name, sdp->host->host_no, ret);
        return -EIO;
    }

    if ((buf[0] & 0x3f) != SCSI_LOG_PAGE_TEMP) {
        pr_loc_dbg("LOG SENSE returned unexpected page 0x%02x (expected 0x%02x) for /dev/%s",
                   buf[0] & 0x3f, SCSI_LOG_PAGE_TEMP, sdp->syno_disk_name);
        return -ENODATA;
    }

    u8 temp = buf[SCSI_LOG_TEMP_BYTE];
    if (temp == SCSI_TEMP_UNAVAILABLE) {
        pr_loc_dbg("LOG SENSE temperature not available (0xFF) for /dev/%s", sdp->syno_disk_name);
        return -ENODATA;
    }

    pr_loc_dbg("LOG SENSE temperature=%d\u00b0C for /dev/%s", (int)temp, sdp->syno_disk_name);
    return (int)temp;
}