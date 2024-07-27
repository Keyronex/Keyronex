#ifndef KRX_DEV_DKAACPIPLATFORM_H
#define KRX_DEV_DKAACPIPLATFORM_H

#include "ddk/DKDevice.h"

typedef struct {
	char Signature[8];
	uint8_t Checksum;
	char OEMID[6];
	uint8_t Revision;
	uint32_t RsdtAddress;
} __attribute__((packed)) rsdp_desc_t;

@interface DKACPIPlatform : DKDevice

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp;
+ (instancetype)instance;

- (void)secondStageInit;

@end

#endif /* KRX_DEV_DKAACPIPLATFORM_H */
