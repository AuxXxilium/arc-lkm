/**
 * Silences boot-time "Invalid parameter" log spam from get_disk_port_type_and_index_by_ata_port()
 *
 * WHY THIS SHIM?
 * Synology's libata-core.c exports get_disk_port_type_and_index_by_ata_port(), which every libata error-handling
 * path (link resets, port scans - see libata-eh.c/libata-scsi.c) calls to resolve which physical bay/slot an
 * ata_port maps to, by walking the kernel's Device Tree (starting from the global `of_root`). On non-DT platforms
 * (i.e. everything running under redpill - this loader boots via ACPI/legacy BIOS, not real DT hardware) `of_root`
 * is always NULL, so the function immediately hits its own parameter-validation branch and logs:
 *     printk("Invalid parameter\n");
 * before returning -1. That's not a real error - a NULL of_root is the expected, permanent state on every platform
 * this loader supports - but because it's called from libata-eh on every port during boot (and again on every link
 * reset/rescan) it floods the kernel log with dozens of context-free "Invalid parameter" lines with no indication
 * of where they came from.
 *
 * HOW DOES IT WORK?
 * We override the exported symbol with a thin shim that special-cases the is_dt=false platforms (i.e. all of them,
 * currently) to return "no DT mapping" immediately, without calling the real function or touching any DT internals -
 * of_root is NULL either way, so this is exactly the value the original function would have computed via its own
 * `NULL == of_root` check anyway, just without the printk. On the off chance a future DT-aware platform is added
 * (is_dt=true), we don't touch behavior at all: the call is passed through to the real, saved-original function
 * pointer unconditionally, so the actual DT-walk logic still runs exactly as shipped.
 *
 * IMPORTANT: the original function is invoked via a raw saved pointer, NOT via call_overridden_symbol()/
 * call_overridden_symbol_void(). Those macros temporarily patch the live sd_ioctl-style trampoline out of the
 * target function's in-memory code (disable override -> call -> re-enable) which is safe only for rare, effectively
 * single-threaded call sites. get_disk_port_type_and_index_by_ata_port() is called concurrently from libata-eh
 * across multiple ata_ports/CPUs during boot and link resets, so doing that dance here would reintroduce the same
 * multi-CPU code-patching race already found and fixed in smart_shim.c's sd_ioctl_canary(). Reading the original
 * pointer once at registration time and calling it directly has no shared mutable state and is safe under
 * concurrency.
 *
 * References
 *   - drivers/ata/libata-core.c (get_disk_port_type_and_index_by_ata_port, GPL source)
 *   - drivers/ata/libata-eh.c, drivers/ata/libata-scsi.c (callers)
 */
#include "dt_disk_port_shim.h"
#include "../shim_base.h"
#include "../../common.h"
#include "../../internal/helper/symbol_helper.h" //kernel_has_symbol()
#include "../../internal/override/override_symbol.h"
#include <linux/libata.h> //struct ata_port
#include <linux/synolib.h> //DISK_PORT_TYPE

#define SHIM_NAME "DT disk port resolver"
#define SHIMMED_SYMBOL "get_disk_port_type_and_index_by_ata_port"

static override_symbol_inst *ov_disk_port_type = NULL;
static int (*org_disk_port_type)(const struct ata_port *, DISK_PORT_TYPE *, int *) = NULL;
static bool platform_is_dt = false;

static int get_disk_port_type_and_index_by_ata_port_shim(const struct ata_port *ap, DISK_PORT_TYPE *portType,
                                                          int *portIndex)
{
    //DT-aware platforms: never touched, pass through to the real DT-walking implementation unconditionally
    if (unlikely(platform_is_dt))
        return org_disk_port_type(ap, portType, portIndex);

    //Non-DT platforms (everything else): of_root is always NULL here, so the real function's combined
    //"NULL == ap || NULL == of_root || ..." guard always trips regardless of ap/portType/portIndex validity,
    //jumping straight to its "return iRet" with iRet still at its initial -1 and *portType/*portIndex never
    //touched (the *portType = UNKNOWN_DEVICE assignment sits after that guard, so it's unreachable here).
    //Replicate that outcome exactly, minus the log spam.
    return -1;
}

int register_dt_disk_port_shim(const struct hw_config *hw)
{
    shim_reg_in();

    platform_is_dt = hw->is_dt;

    if (unlikely(!kernel_has_symbol(SHIMMED_SYMBOL))) {
        pr_loc_bug("Cannot shim " SHIMMED_SYMBOL "() - symbol not found");
        return -ENXIO;
    }

    ov_disk_port_type = override_symbol(SHIMMED_SYMBOL, get_disk_port_type_and_index_by_ata_port_shim);
    if (unlikely(IS_ERR(ov_disk_port_type))) {
        int out = PTR_ERR(ov_disk_port_type);
        ov_disk_port_type = NULL;
        pr_loc_err("Failed to shim " SHIMMED_SYMBOL "() - error=%d", out);
        return out;
    }
    org_disk_port_type = __get_org_ptr(ov_disk_port_type);

    shim_reg_ok();
    return 0;
}

int unregister_dt_disk_port_shim(void)
{
    shim_ureg_in();

    if (unlikely(!ov_disk_port_type))
        return 0; //noop - never registered (e.g. symbol missing)

    int out = restore_symbol(ov_disk_port_type);
    if (unlikely(out != 0)) {
        pr_loc_err("Failed to restore " SHIMMED_SYMBOL "() - error=%d", out);
        return out;
    }

    ov_disk_port_type = NULL;
    org_disk_port_type = NULL;

    shim_ureg_ok();
    return 0;
}
