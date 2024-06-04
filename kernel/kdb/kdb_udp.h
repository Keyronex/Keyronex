#ifndef KRX_KDB_KDB_UDP_H
#define KRX_KDB_KDB_UDP_H

struct pbuf;
@class DKNIC;

/* returns 1 if debugger entered */
int kdbudp_check_packet();

extern DKNIC *kdb_nic;
extern struct pbuf kdb_udp_rx_pbuf;
extern char kdb_udp_rx_buf[2048];
extern const char *kdb_devname;

#endif /* KRX_KDB_KDB_UDP_H */
