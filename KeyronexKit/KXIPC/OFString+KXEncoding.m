#import <ObjFW/OFString.h>

#import <KeyronexKit/KXEncoder.h>

@implementation
OFString (KXEncoding)

- (void)encodeWithCoder:(KXEncoder *)encoder
{
	[encoder encodeValueOfObjCType:@encode( char *)
				      at:self.UTF8String
				  forKey:"value"];
}

@end
