#!/usr/bin/env python3
"""Compile the historical ioctl path before/after the proposed source patch.

Requires Python 3, a C compiler, git and network access. No devices or root access.
The generated IDENTIFY fixture contains no real serial numbers or user data.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import urllib.request

BASE = "32c18946a362aaba4d6a7dfc4131c0d2623252ef"
URL = f"https://raw.githubusercontent.com/AuxXxilium/arc-lkm/{BASE}/shim/storage/smart_shim.c"


def function(source, name):
    marker = source.index(name + "(")
    # Names also occur in comments; require a definition ending in an opening brace.
    while True:
        line = source.rfind("\n", 0, marker) + 1
        opening = source.find("{", marker)
        if source[line:marker].startswith("static ") and ";" not in source[marker:opening]:
            break
        marker = source.index(name + "(", marker + len(name))
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[line:end]


PRELUDE = r'''
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <scsi/sg.h>
typedef uint8_t u8;
typedef unsigned fmode_t;
#define __user
#define unlikely(x) (x)
#define pr_loc_err(...) ((void)0)
#define pr_loc_bug(...) ((void)0)
#define pr_loc_dbg_ioctl(...) ((void)0)
#define ATA_CMD_ID_ATA 0xec
#define ATA_PASS_THROUGH_12 0xa1
#define ATA_PASS_THROUGH_16 0x85
#define ATA_PT12_CMD_OFFSET 9
#define ATA_PT16_CMD_OFFSET 14
#define HDIO_DRIVE_CMD 0x031f
#define HDIO_DRIVE_TASK 0x031e
struct rp_hd_driveid { u8 before[46], fw_rev[8], after[458]; };
struct block_device { int unused; };
static int copy_from_user(void *d, const void *s, size_t n) { memcpy(d,s,n); return 0; }
static int copy_to_user(void *d, const void *s, size_t n) { memcpy(d,s,n); return 0; }
static u8 fixture[512];
static int driver_result;
static int calls;
static int real_ioctl(struct block_device *b, fmode_t m, unsigned c, unsigned long a) {
    (void)b; (void)m; assert(c == SG_IO); calls++;
    struct sg_io_hdr *hdr = (void *)a;
    if (!driver_result) memcpy(hdr->dxferp, fixture, sizeof(fixture));
    return driver_result;
}
static int (*sd_ioctl_org)(struct block_device *, fmode_t, unsigned, unsigned long) = real_ioctl;
static int handle_hdio_drive_cmd_ioctl(struct block_device *b, fmode_t m, unsigned c, void *a) {
    (void)b;(void)m;(void)c;(void)a; return -ENOTTY;
}
static int handle_hdio_drive_task_ioctl(struct block_device *b, fmode_t m, unsigned c, void *a) {
    (void)b;(void)m;(void)c;(void)a; return -ENOTTY;
}
'''
MAIN = r'''
static unsigned checksum(const u8 *p) { unsigned s=0;for(int i=0;i<512;i++)s+=p[i];return s&255; }
int main(void) {
    const char *versions[] = {"M3CR023", "SN03", "85.00A85", "A B C"};
    for (unsigned v=0;v<sizeof(versions)/sizeof(versions[0]);v++) {
        memset(fixture,0,sizeof(fixture));
        u8 decoded[8];memset(decoded,' ',8);memcpy(decoded,versions[v],strlen(versions[v]));
        for(int i=0;i<8;i++)fixture[46+i]=decoded[i^1];
        fixture[510]=0xa5;fixture[511]=(u8)(0-checksum(fixture));assert(checksum(fixture)==0);
        for(int variant=0;variant<2;variant++) {
            u8 cdb[16]={0};cdb[0]=variant?0xa1:0x85;cdb[variant?9:14]=0xec;
            u8 response[512];memset(response,0xcc,512);
            sg_io_hdr_t hdr={.interface_id='S',.dxfer_direction=SG_DXFER_FROM_DEV,
                .cmd_len=variant?12:16,.cmdp=cdb,.dxfer_len=512,.dxferp=response};
            calls=0;driver_result=0;
            assert(sd_ioctl_smart_shim(NULL,0,SG_IO,(unsigned long)&hdr)==0);assert(calls==1);
#ifdef EXPECT_BUG
            if(v==0) { assert(response[53]==' ');assert(checksum(response)!=0); }
#else
            assert(memcmp(response,fixture,512)==0);assert(checksum(response)==0);
#endif
            /* A failed ioctl must retain both the error and the caller's buffer. */
            memset(response,0xcc,512);u8 saved[512];memcpy(saved,response,512);
            driver_result=-EIO;calls=0;
            assert(sd_ioctl_smart_shim(NULL,0,SG_IO,(unsigned long)&hdr)==-EIO);assert(calls==1);
#ifndef EXPECT_BUG
            assert(memcmp(response,saved,512)==0);
#endif
        }
    }
#ifdef EXPECT_BUG
    puts("BEFORE: reproduced M3CR023 truncation and invalid IDENTIFY checksum (ATA12/16)");
#else
    puts("AFTER: original 512-byte response/checksum and ioctl errors preserved (ATA12/16)");
#endif
    return 0;
}
'''


def main():
    patch = Path(__file__).resolve().parents[1] / "patches/preserve-sg-io-identify.patch"
    with urllib.request.urlopen(URL, timeout=30) as response:
        original = response.read().decode()
    with tempfile.TemporaryDirectory(prefix="arc-identify-") as tmp:
        root = Path(tmp)
        target = root / "shim/storage/smart_shim.c"
        target.parent.mkdir(parents=True)
        target.write_text(original)
        subprocess.run(["git", "apply", "--unsafe-paths", str(patch)], cwd=root, check=True)
        repaired = target.read_text()
        names = ["sanitize_ata_string", "is_ata_identify_passthrough",
                 "sanitize_sg_io_identify_response", "handle_sg_io_ioctl"]
        for label, source in [("before", original), ("after", repaired)]:
            helpers = "\n".join(function(source, name) for name in names) if label == "before" else ""
            code = PRELUDE + helpers + "\n" + function(source, "sd_ioctl_smart_shim") + MAIN
            cfile = root / f"{label}.c"
            binary = root / label
            cfile.write_text(code)
            cmd = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Wno-unused-function", "-O2", str(cfile), "-o", str(binary)]
            if label == "before":
                cmd.append("-DEXPECT_BUG")
            subprocess.run(cmd, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
