#ifndef KRX_KEYRONEXKIT_KXINVOCATION_H
#define KRX_KEYRONEXKIT_KXINVOCATION_H

#import <ObjFW/OFMethodSignature.h>

struct RawArguments {
	void *fptr;

	uint64_t rdi;
	uint64_t rsi;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t r8;
	uint64_t r9;

	uint64_t stackArgsCount;
	uint64_t *stackArgs;

	uint64_t rax_ret;
	uint64_t rdx_ret;

	uint64_t isStretCall;
};

@interface KXInvocation : OFObject {
	OFMethodSignature *_sig;
	struct RawArguments _raw;
	BOOL _argumentsRetained;
	void *_stretBuffer;
}

+ (KXInvocation *)invocationWithMethodSignature:(OFMethodSignature *)sig;

- (instancetype)initWithMethodSignature:(OFMethodSignature *)sig;

- (OFMethodSignature *)methodSignature;

- (void)retainArguments;
- (BOOL)argumentsRetained;

- (id)target;
- (void)setTarget:(id)target;

- (SEL)selector;
- (void)setSelector:(SEL)selector;

- (void *)returnValuePtr;
- (void)getReturnValue:(void *)retLoc;
- (void)setReturnValue:(void *)retLoc;

- (void *)argumentPointerAtIndex:(size_t)idx;
- (void)getArgument:(void *)argumentLocation atIndex:(size_t)idx;
- (void)setArgument:(void *)argumentLocation atIndex:(size_t)idx;

- (void)invoke;
- (void)invokeWithTarget:(id)target;

@end


#endif /* KRX_INCLUDE_KXINVOCATION_H */
