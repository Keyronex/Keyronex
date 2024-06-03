#ifndef KRX_KDB_KDB_UDP_H
#define KRX_KDB_KDB_UDP_H

struct pbuf;

/* returns 1 if debugger entered */
int kdbudp_check_packet(struct pbuf *p);

#endif /* KRX_KDB_KDB_UDP_H */
