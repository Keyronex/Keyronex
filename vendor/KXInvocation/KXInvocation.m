#import <ObjFWRT/ObjFWRT.h>
#import <ObjFW/OFString.h>
#import <ObjFW/OFMutableArray.h>
#import <ObjFW/OFConstantString.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#import "KeyronexKit/KXInvocation.h"

void KXInvocationCall(struct RawArguments *);
void KXInvocationForward(void);
void KXInvocationForwardStret(void);
const char *OFGetSizeAndAlignment(const char *typePtr, size_t *sizep,
    size_t *alignp);

enum TypeClassification {
	TypeNone,
	TypeObject,
	TypeBlock,
	TypeCString,
	TypeInteger,
	TypeTwoIntegers,
	TypeEmptyStruct,
	TypeStruct,
	TypeOther
};

@interface
OFObject (Forwarding)
- (void)forwardInvocation:(KXInvocation *)inv;
@end

@interface KXInvocation (private)
- (BOOL)isStretReturn;
- (size_t)sizeAtIndex:(size_t)idx;
- (const void*)returnValuePointer;
- (size_t)returnValueSize;
- (size_t)sizeOfType:(const char *)type;
- (void)iterateRetainableArguments:(void (*)(KXInvocation *, size_t idx, id obj,
				       id block, char *cstr))block;
- (enum TypeClassification)classifyArgumentAtIndex:(size_t)idx;
- (enum TypeClassification)classifyType:(const char *)type;
- (enum TypeClassification)classifyStructType:(const char *)type;
- (void)enumerateStructElementTypes:(const char *)type
			      block:(void (*)(KXInvocation *, const char *,
					enum TypeClassification *))block
				out:(enum TypeClassification *)classification;
- (BOOL)isIntegerClass:(enum TypeClassification)classification;
- (void *)returnValuePtr;
@end

@implementation KXInvocation

+ (void)initialize
{
	printf("Initialising Forwarding\n");
	objc_setForwardHandler((IMP)KXInvocationForward,
	    (IMP)KXInvocationForwardStret);
}

+ (KXInvocation *)invocationWithMethodSignature:(OFMethodSignature *)sig
{
	return [[[self alloc] initWithMethodSignature:sig] autorelease];
}

- (id)initWithMethodSignature:(OFMethodSignature *)sig
{
	if ((self = [super init])) {
		_sig = [sig retain];

		_raw.isStretCall = [self isStretReturn];

		size_t argsCount = [sig numberOfArguments];
		if (_raw.isStretCall)
			argsCount++;

		if (argsCount > 6) {
			_raw.stackArgsCount = argsCount - 6;
			_raw.stackArgs = calloc(argsCount - 6,
			    sizeof(*_raw.stackArgs));
		}
	}
	return self;
}

static void
do_release(KXInvocation *self, size_t idx, id obj, id block, char *cstr)
{
	[obj release];
	[block release];
	free(cstr);
}

- (void)dealloc
{
	if (_argumentsRetained) {
		[self iterateRetainableArguments:do_release];
	}

	[_sig release];
	free(_raw.stackArgs);

	[super dealloc];
}

- (OFString *)description
{
	OFMutableArray *stackArgsStrings = [OFMutableArray array];
	for (size_t i = 0; i < _raw.stackArgsCount; i++)
		[stackArgsStrings addObject:[OFString stringWithFormat:@"%lx",
						      _raw.stackArgs[i]]];
	OFString *stackArgsString = [stackArgsStrings
	    componentsJoinedByString:@" "];
	return [OFString
	    stringWithFormat:
		@"<%@ %p: rdi=%lx rsi=%lx rdx=%lx rcx=%lx r8=%lx r9=%lx stackArgs=%p(%lx)[%@] rax_ret=%lx rdx_ret=%lx isStretCall=%lx>",
	    [self class], self, _raw.rdi, _raw.rsi, _raw.rdx, _raw.rcx, _raw.r8,
	    _raw.r9, _raw.stackArgs, _raw.stackArgsCount, stackArgsString,
	    _raw.rax_ret, _raw.rdx_ret, _raw.isStretCall];
}

- (OFMethodSignature *)methodSignature
{
	return _sig;
}

static void
do_retain(KXInvocation *self, size_t idx, id obj, id block, char *cstr)
{
	if (obj) {
		[obj retain];
	} else if (block) {
		block = [block copy];
		[self setArgument:&block atIndex:idx];
	} else if (cstr) {
		if (cstr != NULL)
			cstr = strdup(cstr);
		[self setArgument:&cstr atIndex:idx];
	}
}

- (void)retainArguments
{
	if (_argumentsRetained)
		return;

	[self iterateRetainableArguments:do_retain];
	_argumentsRetained = YES;
}

- (BOOL)argumentsRetained
{
	return _argumentsRetained;
}

- (id)target
{
	id target;
	[self getArgument:&target atIndex:0];
	return target;
}

- (void)setTarget:(id)target
{
	[self setArgument:&target atIndex:0];
}

- (SEL)selector
{
	SEL sel;
	[self getArgument:&sel atIndex:1];
	return sel;
}

- (void)setSelector:(SEL)selector
{
	[self setArgument:&selector atIndex:1];
}

- (void)getReturnValue:(void *)retLoc
{
	size_t size = [self returnValueSize];
	memcpy(retLoc, [self returnValuePtr], size);
}

- (void)setReturnValue:(void *)retLoc
{
	size_t size = [self returnValueSize];
	memcpy((void*)[self returnValuePtr], retLoc, size);
}

- (void)getArgument:(void *)argumentLocation atIndex:(size_t)idx
{
	size_t rawArgumentIndex = idx;
	if (_raw.isStretCall)
		rawArgumentIndex++;

	const void *src = [self argumentPointerAtIndex:rawArgumentIndex];
	assert(src);

	size_t size = [self sizeAtIndex:idx];
	memcpy(argumentLocation, src, size);
}

- (void)setArgument:(void *)argumentLocation atIndex:(size_t)idx
{
	size_t rawArgumentIndex = idx;
	if (_raw.isStretCall)
		rawArgumentIndex++;

	void *dest = (void*)[self argumentPointerAtIndex:rawArgumentIndex];
	assert(dest);

	enum TypeClassification c = [self classifyArgumentAtIndex:idx];
	if (_argumentsRetained && c == TypeObject) {
		id old = *(id *)dest;
		*(id *)dest = [*(id *)argumentLocation retain];
		[old release];
	} else if (_argumentsRetained && c == TypeBlock) {
		id old = *(id *)dest;
		*(id *)dest = [*(id *)argumentLocation copy];
		[old release];
	} else if (_argumentsRetained && c == TypeCString) {
		char *old = *(char **)dest;

		char *cstr = *(char **)argumentLocation;
		if (cstr != NULL)
			cstr = strdup(cstr);
		*(char **)dest = cstr;

		free(old);
	} else {
		size_t size = [self sizeAtIndex:idx];
		memcpy(dest, argumentLocation, size);
	}
}

- (void)invoke
{
	[self invokeWithTarget:[self target]];
}

- (void)invokeWithTarget:(id)target
{
	[self setTarget:target];
	_raw.fptr = [target methodForSelector:[self selector]];
	if (_raw.isStretCall)
		_raw.rdi = (uint64_t)[self returnValuePtr];
	KXInvocationCall(&_raw);
}

#pragma mark Private

- (void *)argumentPointerAtIndex:(size_t)idx
{
	uint64_t *ptr = NULL;
	if (idx == 0)
		ptr = &_raw.rdi;
	if (idx == 1)
		ptr = &_raw.rsi;
	if (idx == 2)
		ptr = &_raw.rdx;
	if (idx == 3)
		ptr = &_raw.rcx;
	if (idx == 4)
		ptr = &_raw.r8;
	if (idx == 5)
		ptr = &_raw.r9;
	if (idx >= 6)
		ptr = _raw.stackArgs + idx - 6;
	return ptr;
}

- (size_t)sizeAtIndex:(size_t)idx
{
	return [self sizeOfType:[_sig argumentTypeAtIndex:idx]];
}

- (size_t)returnValueSize
{
	return [self sizeOfType:[_sig methodReturnType]];
}

- (size_t)sizeOfType:(const char *)type
{
	size_t size;
	OFGetSizeAndAlignment(type, &size, NULL);
	return size;
}

- (void)iterateRetainableArguments:(void (*)(KXInvocation *self, size_t idx,
				       id obj, id block, char *cstr))block
{
	for (size_t i = 0; i < [_sig numberOfArguments]; i++) {
		enum TypeClassification c = [self classifyArgumentAtIndex:i];
		if (c == TypeObject || c == TypeBlock) {
			id arg;
			[self getArgument:&arg atIndex:i];

			id o = c == TypeObject ? arg : nil;
			id b = c == TypeBlock ? arg : nil;
			block(self, i, o, b, NULL);
		} else if (c == TypeCString) {
			char *arg;
			[self getArgument:&arg atIndex:i];

			block(self, i, nil, nil, arg);
		}
	}
}

- (enum TypeClassification)classifyArgumentAtIndex:(size_t)idx
{
	return [self classifyType:[_sig argumentTypeAtIndex:idx]];
}

- (enum TypeClassification)classifyType:(const char *)type
{
	const char *idType = @encode(id);
#if 0
	const char *blockType = @encode(void (^)(void));
#endif
	const char *charPtrType = @encode(char *);

	if (strcmp(type, idType) == 0)
		return TypeObject;
#if 0
	if (strcmp(type, blockType) == 0)
		return TypeBlock;
#endif
	if (strcmp(type, charPtrType) == 0)
		return TypeCString;

	char intTypes[] = { @encode(signed char)[0], @encode(unsigned char)[0],
		@encode(short)[0], @encode(unsigned short)[0], @encode(int)[0],
		@encode(unsigned int)[0], @encode(long)[0],
		@encode(unsigned long)[0], @encode(long long)[0],
		@encode(unsigned long long)[0], '?', '^', 0 };
	if (strchr(intTypes, type[0]))
		return TypeInteger;

	if (type[0] == '{')
		return [self classifyStructType:type];

	return TypeOther;
}

static void
do_enumerate_struct_types(KXInvocation *self, const char *type,
    enum TypeClassification *out)
{
	enum TypeClassification elementClassification = [self
	    classifyType:type];
	if (*out == TypeEmptyStruct)
		*out = elementClassification;
	else if ([self isIntegerClass:*out] &&
	    [self isIntegerClass:elementClassification])
		*out = TypeTwoIntegers;
	else
		*out = TypeStruct;
}

- (enum TypeClassification)classifyStructType:(const char *)type
{
	enum TypeClassification structClassification = TypeEmptyStruct;
	[self enumerateStructElementTypes:type
				    block:do_enumerate_struct_types
				      out:&structClassification];
	return structClassification;
}

- (BOOL)isIntegerClass:(enum TypeClassification)classification
{
	return classification == TypeObject || classification == TypeBlock ||
	    classification == TypeCString || classification == TypeInteger;
}

- (void)enumerateStructElementTypes:(const char *)type
			      block:(void (*)(KXInvocation *, const char *,
					enum TypeClassification *))block
				out:(enum TypeClassification *)classification
{
	const char *equals = strchr(type, '=');
	const char *cursor = equals + 1;
	while (*cursor != '}') {
		block(self, cursor, classification);
		cursor = OFGetSizeAndAlignment(cursor, NULL, NULL);
	}
}

- (BOOL)isStretReturn
{
	return [self classifyType:[_sig methodReturnType]] == TypeStruct;
}

- (void *)returnValuePtr
{
	if (_raw.isStretCall) {
		if (_stretBuffer == NULL)
			_stretBuffer = calloc(1, [self returnValueSize]);
		return _stretBuffer;
	} else {
		return &_raw.rax_ret;
	}
}

void
KXInvocationForwardC(struct RawArguments *r)
{
	id obj;
	SEL sel;

	if (r->isStretCall) {
		obj = (id)r->rsi;
		sel = (SEL)r->rdx;
	} else {
		obj = (id)r->rdi;
		sel = (SEL)r->rsi;
	}

	printf(
	    "< rdi=%lx rsi=%lx rdx=%lx rcx=%lx r8=%lx r9=%lx stackArgs=%p(%lx) rax_ret=%lx rdx_ret=%lx isStretCall=%lx>\n",
	    r->rdi, r->rsi, r->rdx, r->rcx, r->r8, r->r9, r->stackArgs,
	    r->stackArgsCount, r->rax_ret, r->rdx_ret, r->isStretCall);

	OFMethodSignature *sig = [obj methodSignatureForSelector:sel];
	if (sig == nil)
		errx(EXIT_FAILURE, "Class <%s>: No such selector @%s\n", class_getName([obj class]), sel_getName(sel));

	KXInvocation *inv = [[KXInvocation alloc] initWithMethodSignature:sig];
	inv->_raw.rdi = r->rdi;
	inv->_raw.rsi = r->rsi;
	inv->_raw.rdx = r->rdx;
	inv->_raw.rcx = r->rcx;
	inv->_raw.r8 = r->r8;
	inv->_raw.r9 = r->r9;
	// inv->_raw.isStretCall =r->isStretCall;

	memcpy(inv->_raw.stackArgs, r->stackArgs,
	    inv->_raw.stackArgsCount * sizeof(uint64_t));

	[obj forwardInvocation:(id)inv];

	r->rax_ret = inv->_raw.rax_ret;
	r->rdx_ret = inv->_raw.rdx_ret;

	if (r->isStretCall && inv->_stretBuffer) {
		memcpy((void *)r->rdi, inv->_stretBuffer,
		    [inv returnValueSize]);
	}

	[inv release];
}

@end
