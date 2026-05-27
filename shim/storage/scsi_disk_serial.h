#ifndef REDPILL_SCSI_DISK_SERIAL_H
#define REDPILL_SCSI_DISK_SERIAL_H

#include <linux/types.h>

const char *rp_fetch_block_serial(const char *blk_name);
bool rp_fetch_block_fwrev(const char *blk_name, char *fw_rev_out, size_t fw_rev_len);

#endif // REDPILL_SCSI_DISK_SERIAL_H
