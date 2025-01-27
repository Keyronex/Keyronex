#ifndef KRX_DDK_DKDEVICE_H
#define KRX_DDK_DKDEVICE_H

#include "DKObject.h"
#include "kdk/kern.h"
#include "kdk/queue.h"
#include "kdk/dev.h"

#ifndef AMIGA68k
#define kAnsiYellow "\e[0;33m"
#define kAnsiReset "\e[0m"
#else
#define kAnsiYellow ""
#define kAnsiReset ""
#endif

#define DKPrint(...) kprintf(__VA_ARGS__)

#define DKLog(SUB, ...)({                            \
	kprintf(kAnsiYellow "%s: " kAnsiReset, SUB); \
	kprintf(__VA_ARGS__);                        \
})

#define DKLogAttach(DEV) \
	kprintf(kAnsiYellow "%s" kAnsiReset " at " kAnsiYellow	\
			       "%s" kAnsiReset "\n",		\
	    [DEV devName],					\
	    [DEV provider] ? [[DEV provider] devName] : "(root)");

#define DKLogAttachExtra(DEV, FMT, ...) \
	kprintf(kAnsiYellow "%s" kAnsiReset " at " kAnsiYellow	 \
			       "%s: " kAnsiReset FMT "\n",	 \
	    [DEV devName],					 \
	    [DEV provider] ? [[DEV provider] devName] : "(root)",\
	    ##__VA_ARGS__)

#define DKDevLog(DEV, ...) DKLog([DEV devName], __VA_ARGS__)

@class DKDevice;

typedef TAILQ_TYPE_HEAD(, DKDevice) DKDevice_queue_t;
typedef TAILQ_TYPE_ENTRY(DKDevice) DKDevice_queue_entry_t;

@interface DKDevice : DKOBObject {
    @public
	/*! Providing device; next level in tree. Retained. */
	DKDevice *m_provider;
	/*! Depth of the device stack; 1 for root, 2 for next level, etc. */
	size_t m_stackDepth;
	/*! Queue of subdevices - entries on this queue are retained. */
	DKDevice_queue_t m_subDevices;
	/*! Entry in m_provider's m_subDevices queue. */
	DKDevice_queue_entry_t m_subDevices_entry;
}

+ (const char *)devName;

/*! returns unretained! */
- (const char *)devName;
/*! returns unretained! */
- (DKDevice *)provider;

/*! Initialise with a given provider. */
- (instancetype)initWithProvider:(DKDevice *)provider;

/*!
 * Register the device in the system; add it to its parent provider.
 * This is expected to be called from within the device's init method.
 * It will retain the device.
 */
- (void)registerDevice;

- (iop_return_t)completeIOP:(iop_t *)iop;
- (iop_return_t)dispatchIOP:(iop_t *)iop;

@end

@class DKPlatformInterruptController;

typedef struct dk_interrupt_source {
	DKPlatformInterruptController *pic;
	uint32_t id;
	bool low_polarity;
	bool edge;
} dk_interrupt_source_t;

@protocol DKPlatformInterruptControl

- (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry;

@end

@protocol DKPlatformDevice
/*!
 * Probe for the platform device. Should always return YES. It is expected that
 * a structure shared between PAC and the platform device as a global will be
 * used to communicate information between them.
 */
+ (BOOL)probe;

/*!
 * Second-stage initialisation for the platform device. This is called in thread
 * context after SMP and all other essentials are set up.
 *
 */
- (void)secondStageInit;

/*!
 * Get the platform interrupt controller for the system.
 */
- (DKDevice<DKPlatformInterruptControl> *)platformInterruptController;

@end

extern DKDevice <DKPlatformDevice> *platformDevice;

#endif /* KRX_DDK_DKDEVICE_H */
