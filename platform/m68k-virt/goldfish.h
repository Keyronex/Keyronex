#ifndef KRX_VIRT68K_GOLDFISH_H
#define KRX_VIRT68K_GOLDFISH_H

#include "kdk/kern.h"

#define PIC_IRQ(pic, irq) (((pic)-1) * 32 + ((irq)-1))

void gfpic_dispatch(unsigned int pic_num, md_intr_frame_t *frame);
void gfpic_unmask_irq(unsigned int vector);
void gfpic_handle_irq(unsigned int vector,
    bool (*handler)(md_intr_frame_t *, void *), void *arg);

void gfrtc_init(void);
uint64_t gfrtc_get_time(void);
void gfrtc_oneshot(uint64_t ns);

void gftty_init(void);

#endif /* KRX_VIRT68K_GOLDFISH_H */
