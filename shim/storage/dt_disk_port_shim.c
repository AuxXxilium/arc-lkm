/**
 * Silences boot-time "Invalid parameter" log spam from get_disk_port_type_and_index_by_ata_port()
 *
 * WHY THIS SHIM?
 * Synology's libata-core.c exports get_disk_port_type_and_index_by_ata_port(), which every libata error-handling
 * path (link resets, port scans - see libata-eh.c/libata-scsi.c) calls to resolve which physical bay/slot an
 * ata_port maps to, by walking the kernel's Device Tree starting from the global `of_root`. Under redpill `of_root`
 * is always NULL regardless of which physical platform is being emulated (hw_config.is_dt reflects whether the real
 * hardware being emulated normally ships a DT blob - e.g. epyc7002/SA6400 has is_dt=true - but this loader boots via
 * ACPI/legacy BIOS/hypervisor, which never populates an actual `of_root` DT blob at all). So on every redpill boot,
 * DT-flagged platforms included, the function immediately hits its own parameter-validation branch and logs:
 *     printk("Invalid parameter\n");
 * before returning -1. That's not a real error - a NULL of_root is the expected, permanent state on every platform
 * this loader supports, DT-flagged or not - but because it's called from libata-eh on every port during boot (and
 * again on every link reset/rescan) it floods the kernel log with dozens of context-free "Invalid parameter" lines
 * with no indication of where they came from.
 *
 * HOW DOES IT WORK?
 * We override the exported symbol with a thin shim that checks the actual `of_root` kernel global directly (the
 * same one the real function checks) rather than hw_config.is_dt - the two are different things: is_dt describes
 * the physical hardware being emulated, of_root describes whether a device tree is actually present *in this boot*.
 * When of_root is NULL we return "no DT mapping" immediately, without calling the real function - this is exactly
 * the value the original function would have computed via its own `NULL == of_root` check anyway, just without the
 * printk. If a real of_root ever exists (e.g. this loader gains genuine DT-boot support in the future), the call is
 * passed through to the real, saved-original function pointer unconditionally, so the actual DT-walk logic still
 * runs exactly as shipped.
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
 *   - drivers/of/base.c (of_root; EXPORT_SYMBOL'd, declared extern in include/linux/of.h)
 */
#include "dt_disk_port_shim.h"
#include "../shim_base.h"
#include "../../common.h"
#include "../../internal/helper/symbol_helper.h" //kernel_has_symbol()
#include "../../internal/override/override_symbol.h"
#include <linux/libata.h> //struct ata_port
#include <linux/synolib.h> //DISK_PORT_TYPE
#include <linux/of.h> //of_root

#define SHIM_NAME "DT disk port resolver"
#define SHIMMED_SYMBOL "get_disk_port_type_and_index_by_ata_port"

static override_symbol_inst *ov_disk_port_type = NULL;
static int (*org_disk_port_type)(const struct ata_port *, DISK_PORT_TYPE *, int *) = NULL;

static int get_disk_port_type_and_index_by_ata_port_shim(const struct ata_port *ap, DISK_PORT_TYPE *portType,
                                                          int *portIndex)
{
    //A real of_root (genuine DT boot): never touched, pass through to the real DT-walking implementation
    if (unlikely(of_root))
        return org_disk_port_type(ap, portType, portIndex);

    //of_root is NULL (every redpill boot, on every platform): the real function's combined
    //"NULL == ap || NULL == of_root || ..." guard always trips regardless of ap/portType/portIndex validity,
    //jumping straight to its "return iRet" with iRet still at its initial -1 and *portType/*portIndex never
    //touched (the *portType = UNKNOWN_DEVICE assignment sits after that guard, so it's unreachable here).
    //Replicate that outcome exactly, minus the log spam.
    return -1;
}

int register_dt_disk_port_shim(void)
{
    shim_reg_in();

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
