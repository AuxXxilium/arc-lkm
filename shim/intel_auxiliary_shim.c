/*
 * Provides intel_auxiliary_* symbol aliases required by Synology's out-of-tree
 * i40e.ko, which was compiled against Intel's downstream auxiliary-bus fork.
 *
 * auxiliary.ko exports auxiliary_device_init() and __auxiliary_device_add().
 * i40e.ko references intel_auxiliary_device_init() and intel___auxiliary_device_add().
 *
 * We can't call auxiliary.ko functions directly at link time — auxiliary.ko loads
 * ~60s into DSM boot, long after redpill.ko loads at ~4s. Instead we resolve the
 * real symbols lazily via kln_func (the kallsyms_lookup_name wrapper) at first call,
 * by which time auxiliary.ko is guaranteed to be resident.
 */
#include "../internal/helper/symbol_helper.h"
#include <linux/module.h>
#include <linux/errno.h>

/* Forward declaration only — we never dereference this struct ourselves.
 * The pointer is passed opaquely to the real auxiliary_device_init() /
 * __auxiliary_device_add() resolved at runtime via kallsyms, so we don't
 * need linux/auxiliary_bus.h (which may not be present in the build env). */
struct auxiliary_device;

static int (*_auxiliary_device_init_fn)(struct auxiliary_device *) = NULL;
static int (*___auxiliary_device_add_fn)(struct auxiliary_device *, const char *) = NULL;

int intel_auxiliary_device_init(struct auxiliary_device *auxdev)
{
    if (unlikely(!_auxiliary_device_init_fn)) {
        _auxiliary_device_init_fn =
            (int (*)(struct auxiliary_device *))kln_func("auxiliary_device_init");
        if (!_auxiliary_device_init_fn)
            return -ENOSYS;
    }
    return _auxiliary_device_init_fn(auxdev);
}
EXPORT_SYMBOL(intel_auxiliary_device_init);

int intel___auxiliary_device_add(struct auxiliary_device *auxdev, const char *modname)
{
    if (unlikely(!___auxiliary_device_add_fn)) {
        ___auxiliary_device_add_fn =
            (int (*)(struct auxiliary_device *, const char *))kln_func("__auxiliary_device_add");
        if (!___auxiliary_device_add_fn)
            return -ENOSYS;
    }
    return ___auxiliary_device_add_fn(auxdev, modname);
}
EXPORT_SYMBOL(intel___auxiliary_device_add);
