#ifndef REDPILL_DT_DISK_PORT_SHIM_H
#define REDPILL_DT_DISK_PORT_SHIM_H

#include "../../config/platform_types.h" //struct hw_config

int register_dt_disk_port_shim(const struct hw_config *hw);
int unregister_dt_disk_port_shim(void);

#endif //REDPILL_DT_DISK_PORT_SHIM_H
