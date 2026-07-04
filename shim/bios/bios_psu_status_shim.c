/**
 * Overrides HWMONGetPSUStatusByI2C and all known per-model *I2CGetPowerInfo symbols to provide fake dual-PSU status
 *
 * Dual (redundant) PSU models each expose their own "<Model>I2CGetPowerInfo" symbol which mfgBIOS's
 * redundant_power_check calls directly (bypassing the normal VTK_GET_HWMON_PSU_STATUS vtable entry). Since the
 * symbol name is model-specific there's no single function to override - instead we keep a table of known names
 * and override whichever ones actually exist in the currently loaded mfgBIOS (kernel_has_symbol()), so adding
 * support for a new dual-PSU model is just adding its symbol name to KNOWN_I2C_POWER_INFO_SYMBOLS below.
 *
 * To find a new model's symbol name consult the mfgbios output while booting the dev LKM.
 */
#include "bios_psu_status_shim.h"
#include "../../common.h"
#include "../shim_base.h"
#include "../../internal/override/override_symbol.h" //overriding HWMONGetPSUStatusByI2C
#include "../../internal/helper/symbol_helper.h"      //kernel_has_symbol()
#include "../../config/platform_types.h"              //hw_config, platform_has_hwmon_*
#include <linux/synobios.h>                           //CAPABILITY_*, CAPABILITY

#define SHIM_NAME "mfgBIOS HWMONGetPSUStatusByI2C"

//Known per-model dual-PSU I2C power info symbols; add new models here as they're discovered
static const char *KNOWN_I2C_POWER_INFO_SYMBOLS[] = {
    "RS4021xspI2CGetPowerInfo",
    "RS4022xspI2CGetPowerInfo",
    "RS4023xspI2CGetPowerInfo",
    "RS4024xspI2CGetPowerInfo",
    "FS3410I2CGetPowerInfo",
    "FS6400I2CGetPowerInfo",
    "FS6500I2CGetPowerInfo",
    "HD6500I2CGetPowerInfo",
    "SA6400I2CGetPowerInfo",
    "SA3400I2CGetPowerInfo",
    "SA3600I2CGetPowerInfo",
    "SA3410I2CGetPowerInfo",
    "SA3610I2CGetPowerInfo",
    "FS2017I2CGetPowerInfo",
    "FS3400I2CGetPowerInfo",
    "FS1018I2CGetPowerInfo",
    "FS3600I2CGetPowerInfo",
    "FS2500I2CGetPowerInfo",
};
#define KNOWN_I2C_POWER_INFO_SYMBOLS_NUM (sizeof(KNOWN_I2C_POWER_INFO_SYMBOLS) / sizeof(KNOWN_I2C_POWER_INFO_SYMBOLS[0]))

static const struct hw_config *hw_config = NULL;
static override_symbol_inst *HWMONGetPSUStatusByI2C_ovs = NULL;
static override_symbol_inst *i2c_power_info_ovs[KNOWN_I2C_POWER_INFO_SYMBOLS_NUM] = { NULL };
static int i2c_power_info_ovs_num = 0; //how many entries in i2c_power_info_ovs are actually populated

static int HWMONGetPSUStatusByI2C_shim(void)
{
    return 0;
}

static int HWMONI2CGetPowerInfo_shim(void)
{
    return 0;
}

int register_bios_psu_status_shim(const struct hw_config *hw)
{
    shim_reg_in();

    if (unlikely(HWMONGetPSUStatusByI2C_ovs || i2c_power_info_ovs_num))
        shim_reg_already();

    hw_config = hw;
    override_symbol_or_exit_int(HWMONGetPSUStatusByI2C_ovs, "HWMONGetPSUStatusByI2C", HWMONGetPSUStatusByI2C_shim);

    for (int i = 0; i < KNOWN_I2C_POWER_INFO_SYMBOLS_NUM; i++) {
        if (!kernel_has_symbol(KNOWN_I2C_POWER_INFO_SYMBOLS[i]))
            continue;

        override_symbol_inst *ovs = override_symbol(KNOWN_I2C_POWER_INFO_SYMBOLS[i], HWMONI2CGetPowerInfo_shim);
        if (unlikely(IS_ERR(ovs))) {
            pr_loc_err("Failed to override %s - error=%ld", KNOWN_I2C_POWER_INFO_SYMBOLS[i], PTR_ERR(ovs));
            continue; //a single model's symbol failing to shim shouldn't abort shimming the rest
        }

        i2c_power_info_ovs[i2c_power_info_ovs_num++] = ovs;
        pr_loc_dbg("Shimmed dual-PSU symbol %s", KNOWN_I2C_POWER_INFO_SYMBOLS[i]);
    }

    shim_reg_ok();
    return 0;
}

int unregister_bios_psu_status_shim(void)
{
    int out;
    shim_ureg_in();

    if (unlikely(!HWMONGetPSUStatusByI2C_ovs))
        return 0; // this is deliberately a noop

    out = restore_symbol(HWMONGetPSUStatusByI2C_ovs);
    if (unlikely(out != 0)) {
        pr_loc_err("Failed to restore HWMONGetPSUStatusByI2C_ovs - error=%d", out);
        return out;
    }
    HWMONGetPSUStatusByI2C_ovs = NULL;

    for (int i = 0; i < i2c_power_info_ovs_num; i++) {
        out = restore_symbol(i2c_power_info_ovs[i]);
        if (unlikely(out != 0)) {
            pr_loc_err("Failed to restore i2c_power_info_ovs[%d] - error=%d", i, out);
            return out;
        }
        i2c_power_info_ovs[i] = NULL;
    }
    i2c_power_info_ovs_num = 0;

    shim_ureg_ok();
    return 0;
}

int reset_bios_psu_status_shim(void)
{
    shim_reset_in();

    if (HWMONGetPSUStatusByI2C_ovs) {
        put_overridden_symbol(HWMONGetPSUStatusByI2C_ovs);
        HWMONGetPSUStatusByI2C_ovs = NULL;
    }

    for (int i = 0; i < i2c_power_info_ovs_num; i++) {
        put_overridden_symbol(i2c_power_info_ovs[i]);
        i2c_power_info_ovs[i] = NULL;
    }
    i2c_power_info_ovs_num = 0;

    shim_reset_ok();
    return 0;
}
