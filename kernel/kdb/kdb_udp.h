#ifndef KRX_KDB_KDB_UDP_H
#define KRX_KDB_KDB_UDP_H

struct pbuf;

/* returns 1 if debugger entered */
int kdbudp_check_packet();

extern struct pbuf kdb_udp_rx_pbuf;
extern char kdb_udp_rx_buf[2048];

#endif /* KRX_KDB_KDB_UDP_H */
