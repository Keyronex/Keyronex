#ifndef _KXDECODER_H
#define _KXDECODER_H

#import <KeyronexKit/KXIPCConnection.h>
#import <ObjFW/OFObject.h>
#include <dxf/dxf.h>

@interface KXDecoder : OFObject {
	dxf_t *_dxf;
	KXIPCConnection *_connection;
}

@property (retain) KXIPCConnection *connection;

- (instancetype)initWithConnection:(KXIPCConnection *)connection
			       dxf:(dxf_t *)dxf;

/*!
 * @brief Decode the given object.
 */
- (id)decodeObject;

/*!
 * @brief Decode a given object or data for the given key.
 */
- (void)decodeValueOfObjCType:(const char *)type
			   at:(void *)location
		       forKey:(const char *)key;
/*!
 * @brief Decode a given element of an array at the given key.
 */
- (void)decodeValueOfObjCType:(const char *)type
			   at:(void *)location
	     atArraySubscript:(size_t)index
		       forKey:(const char *)key;

@end

#endif /* _KXDECODER_H */
