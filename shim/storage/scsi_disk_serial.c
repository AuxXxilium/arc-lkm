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

struct temp_lookup_ctx {
    const char *blk_name;
    int temp;
    bool found;
};

static int find_matching_serial(struct scsi_device *sdp, void *data)
{
    struct serial_lookup_ctx *ctx = data;

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

static int find_matching_fwrev(struct scsi_device *sdp, void *data)
{
    struct fwrev_lookup_ctx *ctx = data;

    if (strcmp(ctx->blk_name, sdp->syno_disk_name) != 0)
        return 0;

    if (!has_usable_text_bounded(sdp->rev, sizeof(sdp->rev))) {
        pr_loc_dbg("Matched /dev/%s on host%d but firmware revision is empty", sdp->syno_disk_name,
                   sdp->host->host_no);
        return 1;
    }

    scnprintf(ctx->fw_rev_out, ctx->fw_rev_len, "%.*s", (int)sizeof(sdp->rev), sdp->rev);

    /* Trim trailing spaces, nulls, and non-printable bytes left by SCSI inquiry padding */
    {
        char *p = ctx->fw_rev_out + strlen(ctx->fw_rev_out);
        while (p > ctx->fw_rev_out && (!isprint((unsigned char)p[-1]) || p[-1] == ' '))
            *--p = '\0';
    }

    ctx->found = true;
    pr_loc_dbg("Matched /dev/%s on host%d with fw rev '%s'", sdp->syno_disk_name, sdp->host->host_no,
               ctx->fw_rev_out);
    return 1;
}

static int find_and_read_temp(struct scsi_device *sdp, void *data)
{
    struct temp_lookup_ctx *ctx = data;

    if (strcmp(ctx->blk_name, sdp->syno_disk_name) != 0)
        return 0;

    ctx->found = true;
    ctx->temp = scsi_read_disk_temp(sdp);
    return 1; /* stop iteration */
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

    out = for_each_scsi_disk_ctx(find_matching_serial, &ctx);

    /* for_each_scsi_disk_ctx() returns first non-zero callback code; positive means "match found". */
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
    out = for_each_scsi_disk_ctx(find_matching_fwrev, &ctx);

    /* for_each_scsi_disk_ctx() returns first non-zero callback code; positive means "match found". */
    if (unlikely(out < 0 && out != -ENXIO)) {
        pr_loc_err("Failed to enumerate SCSI disks while looking up /dev/%s fw rev - error=%d", blk_name, out);
        return false;
    }

    return ctx.found;
}

int rp_fetch_block_temp(const char *blk_name)
{
    struct temp_lookup_ctx ctx = {
        .blk_name = blk_name,
        .temp = -ENODATA,
        .found = false,
    };
    int out;

    if (unlikely(!blk_name || blk_name[0] == '\0'))
        return -EINVAL;

    out = for_each_scsi_disk_ctx(find_and_read_temp, &ctx);

    /* for_each_scsi_disk_ctx() returns first non-zero callback code; positive means "match found". */
    if (unlikely(out < 0 && out != -ENXIO)) {
        pr_loc_err("Failed to enumerate SCSI disks while reading temp for /dev/%s - error=%d", blk_name, out);
        return -EIO;
    }

    return ctx.found ? ctx.temp : -ENODATA;
}
