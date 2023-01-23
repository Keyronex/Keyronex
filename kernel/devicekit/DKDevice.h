#ifndef DK_DKDEVICE_H_
#define DK_DKDEVICE_H_

#include <sys/queue.h>

#include <OFObject.h>

#define kAnsiYellow "\e[0;33m"
#define kAnsiReset "\e[0m"

#define DKPrint(...) kprintf(__VA_ARGS__)
#define DKLog(SUB, ...)                                      \
	({                                                   \
		kprintf(kAnsiYellow "%s: " kAnsiReset, SUB); \
		kprintf(__VA_ARGS__);                        \
	})
#define DKLogAttach(DEV)                                                       \
	kprintf(kAnsiYellow "%s" kAnsiReset " at " kAnsiYellow "%s" kAnsiReset \
			    "\n",                                              \
	    [DEV name], [DEV provider] ? [[DEV provider] name] : "(root)");
#define DKLogAttachExtra(DEV, FMT, ...)                        \
	kprintf(kAnsiYellow "%s" kAnsiReset " at " kAnsiYellow \
			    "%s: " kAnsiReset FMT "\n",        \
	    [DEV name], [[DEV provider] name], ##__VA_ARGS__);
#define DKDevLog(DEV, ...) DKLog([DEV name], __VA_ARGS__)

@class DKDevice;

struct dk_device_pci_info;

/*! Represenst an offset in unit blocks of an underlying block device. */
typedef int64_t blkoff_t;

typedef _TAILQ_HEAD(, DKDevice, ) DKDevice_queue_t;
typedef _TAILQ_ENTRY(DKDevice, ) DKDevice_queue_entry_t;
#if 0
typedef _SLIST_HEAD(, DKDevice, ) DKDevice_slist_t;
typedef _SLIST_ENTRY(DKDevice, ) DKDevice_slist_entry_t;
#endif

/*!
 * Represents any sort of device, whether physical or pseudo.
 */
@interface DKDevice : OFObject {
	@public
	char		  *m_name;
	DKDevice		 *m_provider;
	DKDevice_queue_t       m_subDevices;
	DKDevice_queue_entry_t m_subDevices_entry;
}

/*! The device's unique name. */
@property (readonly) const char *name;

/*! The device's provider; unless this is the root device. */
@property (readonly) DKDevice *provider;

/*! Initialise with a given provider. */
- initWithProvider:(DKDevice *)provider;

/** Register the device in the system. */
- (void)registerDevice;

@end

#endif /* DKDEVICE_H_ */
