#ifndef KRX_DDK_DKOBJECT_H
#define KRX_DDK_DKOBJECT_H

#include "ObjFWRT.h"

__attribute__((__objc_root_class__))
@interface DKObject {
	Class _isa;
}

+ (void)load;
+ (void)unload;
+ (void)initialize;

+ (instancetype)alloc;

+ (Class)class;
+ (const char *)className;

- (void)dealloc;
- (instancetype)init;
- (instancetype)retain;
- (void)release;

- (const char *)className;

@end

/*! @brief Object Manager-managed object. */
@interface DKOBObject : DKObject

@end


#endif /* KRX_DDK_DKOBJECT_H */
