#include <dev/LimineFB.h>
#include <limine.h>

@implementation LimineFB

@synthesize width, height, pitch, bpp, base;

static int fbNum = 0;
LimineFB  *sysfb = NULL;

+ (BOOL)probeWithProvider:(DKDevice *)provider
	 limineFBResponse:(struct limine_framebuffer_response *)resp
{
	LimineFB *fbs[resp->framebuffer_count];
	assert(resp->framebuffer_count > 0);
	for (int i = 0; i < resp->framebuffer_count; i++)
		fbs[i] = [[self alloc] initWithProvider:provider
					       limineFB:resp->framebuffers[i]];
	sysfb = fbs[0];
	return YES;
}

- initWithProvider:(DKDevice *)provider
	  limineFB:(struct limine_framebuffer *)fb;
{
	self = [super initWithProvider:provider];
	kmem_asprintf(&m_name, "LimineFB%d", fbNum++);
	[self registerDevice];
	width = fb->width;
	height = fb->height;
	pitch = fb->pitch;
	bpp = fb->bpp;
	base = fb->address;
	DKLogAttachExtra(self, " %lux%lux%d", width, height, bpp);
	return self;
}

@end
