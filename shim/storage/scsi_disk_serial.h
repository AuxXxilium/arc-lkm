#ifndef REDPILL_SCSI_DISK_SERIAL_H
#define REDPILL_SCSI_DISK_SERIAL_H

#include <linux/types.h>

const char *rp_fetch_block_serial(const char *blk_name);
bool rp_fetch_block_fwrev(const char *blk_name, char *fw_rev_out, size_t fw_rev_len);

/**
 * Attempts to read the current temperature of a block device via SCSI LOG SENSE
 *
 * @param blk_name block device name, e.g. "sda"
 * @return temperature in Celsius (>= 0) on success, -ENODATA if unavailable, -EIO/-EINVAL on error
 */
int rp_fetch_block_temp(const char *blk_name);

#endif // REDPILL_SCSI_DISK_SERIAL_H
