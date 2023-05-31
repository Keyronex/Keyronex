#import <KeyronexKit/KXDecoder.h>
#import <KeyronexKit/KXEncoder.h>
#import <KeyronexKit/KXInvocation.h>
#import <ObjFW/OFMethodSignature.h>
#import <ObjFW/OFString.h>
#include <assert.h>
#include <err.h>

@implementation
KXInvocation (KXEncoding)

- (size_t)count
{
	return [_sig numberOfArguments];
}

- (void)getElementLocation:(const void **)loc
	  andTypeSignature:(const char **)sig
			at:(size_t)i
{
	*sig = [_sig argumentTypeAtIndex:i];
	*loc = [self argumentPointerAtIndex:i];
}

- (void)encodeWithCoder:(KXEncoder *)encoder
{
	[encoder encodeValuesOfObjCTypesInArray:self forKey:"arguments"];
}

- (instancetype)initWithCoder:(KXDecoder *)coder
{
	id target;
	SEL selector;
	OFMethodSignature *sig;

	[coder decodeValueOfObjCType:@encode(id)
				  at:&target
		    atArraySubscript:0
			      forKey:"arguments"];

	[coder decodeValueOfObjCType:@encode(SEL)
				  at:&selector
		    atArraySubscript:1
			      forKey:"arguments"];

	OFLog(@"Deserialise first part of OFInvocation:"
	       "\n\tSelector: %s"
	       "\n\tTarget: %@"
	       "\nNow looking up method signature....",
	    sel_getName(selector), target);

	sig = [target methodSignatureForSelector:selector];
	OFLog(@"Method signature: %@", sig);

	self = [self initWithMethodSignature:sig];
	assert(self != nil);

	[self setTarget:target];
	[self setSelector:selector];

	for (size_t i = 2; i < [sig numberOfArguments]; i++) {
		[coder decodeValueOfObjCType:[sig argumentTypeAtIndex:i]
					  at:[self argumentPointerAtIndex:i]
			    atArraySubscript:i
				      forKey:"arguments"];
	}

	OFLog(@"Fully decoded:\n%@", self);

	return self;
}

@end
