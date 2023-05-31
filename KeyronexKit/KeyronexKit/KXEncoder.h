#ifndef _KXEncoder_H
#define _KXEncoder_H

#import <ObjFW/OFObject.h>

#include <dxf/dxf.h>
#include <KeyronexKit/KXInvocation.h>

@class KXEncoder;
@class KXInvocation;

@protocol KXArrayEncoding
- (size_t)count;
- (void)getElementLocation:(const void **)loc
          andTypeSignature:(const char **)sig
                        at:(size_t)i;
@end

@interface OFObject (Coding)
- (void)encodeWithCoder:(KXEncoder *)encoder;
@end

@interface KXInvocation (KXEncoding) <KXArrayEncoding>
- (void)encodeWithCoder:(KXEncoder *)encoder;
@end

@interface KXEncoder : OFObject {
  dxf_t *_dxf;
}

- (instancetype)init;

/*!
 * @brief Steal the DXF object created by the encoder.
 */
- (dxf_t *)take;

/*!
 * @brief Encode the given object.
 */
- (void)encodeObject:(id)anObject;

/*!
 * @brief Encode a given object or data for the given key.
 */
- (void)encodeValueOfObjCType:(const char *)type
                           at:(const void *)location
                       forKey:(const char *)key;
/*!
 * @brief Encode (by calling back \p object) an array.
 */
- (void)encodeValuesOfObjCTypesInArray:(id<KXArrayEncoding>)object
                                forKey:(const char *)key;

- (void)dump;

@end

#endif /* _KXEncoder_H */
