#ifndef KRX_DEV_PCI_REG_H
#define KRX_DEV_PCI_REG_H

enum {
	kVendorID = 0x0, /* u16 */
	kDeviceID = 0x2, /* u16 */
	kCommand = 0x4,	 /* u16 */
	kStatus = 0x6,	 /* bit 4 = capabilities list exists */
	kSubclass = 0xa,
	kClass = 0xb,
	kHeaderType = 0xe, /* bit 7 = is multifunction */
	kBaseAddress0 = 0x10,
	kCapabilitiesPointer = 0x34,
	kInterruptPin = 0x3d, /* u8 */
};

enum {
	kCapMSIx = 0x11,
};

#endif /* KRX_DEV_PCI_REG_H */
