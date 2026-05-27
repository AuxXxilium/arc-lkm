#include<scsi/scsi_cmnd.h>
#include<scsi/scsi_device.h>
#include<scsi/scsi_host.h>
#include<linux/ctype.h>
#include<linux/kernel.h>

#include "../../common.h"
#include "../../internal/scsi/scsi_toolbox.h"

static bool has_usable_serial(const char *serial)
{
    if (unlikely(!serial))
        return false;

    for (size_t i = 0; serial[i] != '\0'; ++i) {
        if (!isspace(serial[i]))
            return true;
    }

    return false;
}

static bool has_usable_text_bounded(const char *text, size_t max_len)
{
    if (unlikely(!text))
        return false;

    for (size_t i = 0; i < max_len; ++i) {
        if (text[i] == '\0')
            break;

        if (!isspace(text[i]))
            return true;
    }

    return false;
}

struct serial_lookup_ctx {
    const char *blk_name;
    const char *serial;
};

struct fwrev_lookup_ctx {
    const char *blk_name;
    char *fw_rev_out;
    size_t fw_rev_len;
    bool found;
};

static int find_matching_serial(struct scsi_device *sdp, struct serial_lookup_ctx *ctx)
{
    if (strcmp(ctx->blk_name, sdp->syno_disk_name) != 0)
        return 0;

    if (has_usable_serial(sdp->syno_disk_serial)) {
        pr_loc_dbg("Matched /dev/%s on host%d with serial '%s'", sdp->syno_disk_name, sdp->host->host_no,
                   sdp->syno_disk_serial);
        ctx->serial = sdp->syno_disk_serial;
    } else {
        pr_loc_dbg("Matched /dev/%s on host%d but serial is empty", sdp->syno_disk_name, sdp->host->host_no);
    }

    return 1;
}

static int find_matching_fwrev(struct scsi_device *sdp, struct fwrev_lookup_ctx *ctx)
{
    if (strcmp(ctx->blk_name, sdp->syno_disk_name) != 0)
        return 0;

    if (!has_usable_text_bounded(sdp->rev, sizeof(sdp->rev))) {
        pr_loc_dbg("Matched /dev/%s on host%d but firmware revision is empty", sdp->syno_disk_name,
                   sdp->host->host_no);
        return 1;
    }

    scnprintf(ctx->fw_rev_out, ctx->fw_rev_len, "%.*s", (int)sizeof(sdp->rev), sdp->rev);
    ctx->found = true;
    pr_loc_dbg("Matched /dev/%s on host%d with fw rev '%s'", sdp->syno_disk_name, sdp->host->host_no,
               ctx->fw_rev_out);
    return 1;
}

static struct serial_lookup_ctx *lookup_ctx;
static struct fwrev_lookup_ctx *fwrev_lookup_ctx;

static int find_matching_serial_thunk(struct scsi_device *sdp)
{
    return find_matching_serial(sdp, lookup_ctx);
}

static int find_matching_fwrev_thunk(struct scsi_device *sdp)
{
    return find_matching_fwrev(sdp, fwrev_lookup_ctx);
}

const char *rp_fetch_block_serial(const char *blk_name)
{
    struct serial_lookup_ctx ctx = {
        .blk_name = blk_name,
        .serial = NULL,
    };
    int out;

    if (unlikely(!blk_name || blk_name[0] == '\0'))
        return NULL;

    lookup_ctx = &ctx;
    out = for_each_scsi_disk(find_matching_serial_thunk);
    lookup_ctx = NULL;

    // for_each_scsi_disk() returns first non-zero callback code; positive code here means "match found".
    if (unlikely(out < 0 && out != -ENXIO)) {
        pr_loc_err("Failed to enumerate SCSI disks while looking up /dev/%s serial - error=%d", blk_name, out);
        return NULL;
    }

    return ctx.serial;
}

bool rp_fetch_block_fwrev(const char *blk_name, char *fw_rev_out, size_t fw_rev_len)
{
    struct fwrev_lookup_ctx ctx = {
        .blk_name = blk_name,
        .fw_rev_out = fw_rev_out,
        .fw_rev_len = fw_rev_len,
        .found = false,
    };
    int out;

    if (unlikely(!blk_name || blk_name[0] == '\0' || !fw_rev_out || fw_rev_len == 0))
        return false;

    fw_rev_out[0] = '\0';
    fwrev_lookup_ctx = &ctx;
    out = for_each_scsi_disk(find_matching_fwrev_thunk);
    fwrev_lookup_ctx = NULL;

    // for_each_scsi_disk() returns first non-zero callback code; positive code here means "match found".
    if (unlikely(out < 0 && out != -ENXIO)) {
        pr_loc_err("Failed to enumerate SCSI disks while looking up /dev/%s fw rev - error=%d", blk_name, out);
        return false;
    }

    return ctx.found;
}
