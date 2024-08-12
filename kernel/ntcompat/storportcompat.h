#ifndef KRX_NTCOMPAT_STORPORTCOMPAT_H
#define KRX_NTCOMPAT_STORPORTCOMPAT_H

#include "NTStorPort.h"
#include "ntcompat/win_types.h"
#include "ntcompat.h"
#include "ntcompat/storport.h"

TAILQ_HEAD(storport_driver_queue, storport_driver);

struct sp_dev_ext
{
    NTStorPort *device;
    PPORT_CONFIGURATION_INFORMATION portConfig;
    struct storport_driver *driver;
    NTAPI PHW_PASSIVE_INITIALIZE_ROUTINE passive_init;
    kspinlock_t intxLock;
    uint8_t padding[7];
    uint8_t hw_dev_ext[0];
};

struct storport_driver
{
    TAILQ_ENTRY(storport_driver) queue_entry;
    nt_driver_object_t *nt_driver_object;
    int counter;
    HW_INITIALIZATION_DATA hwinit;
};

/*!
 * @brief This structure aliases STOR_DPC.
 */
struct dpc_alias
{
    kdpc_t *dpc;
    void *devext;
    NTAPI PHW_DPC_ROUTINE dpcfunc;
    void *sysarg1;
    void *sysarg2;
};

@interface NTStorPort (Private)
- (instancetype)initWithPCIBus:(DKPCIBus *)provider
                          info:(struct pci_dev_info *)info
                storportDriver:(struct storport_driver *)driver
               deviceExtension:(struct sp_dev_ext *)devExt;

- (PACCESS_RANGE)getAccessRanges;
- (void)completeSrb:(PSCSI_REQUEST_BLOCK)Srb;
@end

static inline struct sp_dev_ext *
devext_get_sp_devext(PVOID HwDeviceExtension)
{
    char *ptr = (char *)HwDeviceExtension - sizeof(struct sp_dev_ext);
    struct sp_dev_ext *devExt = (struct sp_dev_ext *)ptr;
    return devExt;
}

static inline kspinlock_t *
devext_get_spinlock(PVOID HwDeviceExtension)
{
    char *ptr = (char *)HwDeviceExtension - sizeof(struct sp_dev_ext);
    struct sp_dev_ext *devExt = (struct sp_dev_ext *)ptr;
    return &devExt->intxLock;
}

static inline NTStorPort *
devext_get_self(PVOID HwDeviceExtension)
{
    char *ptr = (char *)HwDeviceExtension - sizeof(struct sp_dev_ext);
    struct sp_dev_ext *devExt = (struct sp_dev_ext *)ptr;
    return devExt->device;
}

extern struct storport_driver_queue drivers;

#endif /* KRX_NTCOMPAT_STORPORTCOMPAT_H */
