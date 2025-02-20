#if 0
#include "dev/DKNIC.h"
#include "dev/safe_endian.h"
#include "kdk/kmem.h"

#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

#define KDB_IP 0x0a00020f /* 10.0.2.15 */

#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806

#define ARP_REQUEST 1
#define ARP_REPLY 2

#define IP_PROTO_UDP 0x11

struct __attribute__((packed)) ethhdr {
	dk_mac_address_t dst;
	dk_mac_address_t src;
	beu16_t type;
};

struct __attribute__((packed)) arphdr {
	beu16_t hwtype;
	beu16_t prototype;
	uint8_t hwlength;
	uint8_t protolength;
	beu16_t op;
	dk_mac_address_t sender_hwaddr;
	beu32_t sender_protaddr;
	dk_mac_address_t target_hwaddr;
	beu32_t target_protaddr;
};

struct __attribute__((packed)) iphdr {
	uint8_t ver_ihl;
	uint8_t tos;
	beu16_t tot_len;
	beu16_t id;
	beu16_t frag_off;
	uint8_t ttl;
	uint8_t protocol;
	beu16_t check;
	beu32_t src_addr;
	beu32_t dst_addr;
};

struct __attribute__((packed)) udphdr {
	beu16_t src_port;
	beu16_t dest_port;
	beu16_t len;
	beu16_t check;
};

dk_mac_address_t kdb_mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
uint16_t kdb_port = 2159;
uint32_t kdb_ip = KDB_IP;

uint16_t kdb_remote_port = -1;
uint32_t kdb_remote_ip = -1;
dk_mac_address_t kdb_remote_mac;
char kdb_udp_rx_buf[2048];
struct pbuf kdb_udp_rx_pbuf = { .payload = kdb_udp_rx_buf };

const char *kdb_devname = "none";

md_intr_frame_t *kdb_saved_frame;

DKNIC *kdb_nic;
bool kdb_active = false;

uint8_t tx_buf[2048];
char received_cmd[1400];

int
do_arp_packet(struct pbuf *p)
{
	struct ethhdr *ethhdr = (struct ethhdr *)p->payload;
	struct arphdr *arphdr = (struct arphdr *)(p->payload +
	    sizeof(struct ethhdr));

	if (from_beu16(ethhdr->type) == ETHERTYPE_ARP &&
	    from_beu16(arphdr->op) == ARP_REQUEST &&
	    from_beu32(arphdr->target_protaddr) == KDB_IP) {

		memcpy(ethhdr->dst, ethhdr->src, 6);
		memcpy(ethhdr->src, kdb_mac, 6);

		arphdr->op = to_beu16(ARP_REPLY);
		memcpy(arphdr->target_hwaddr, arphdr->sender_hwaddr, 6);
		arphdr->target_protaddr = arphdr->sender_protaddr;
		memcpy(arphdr->sender_hwaddr, kdb_mac, 6);
		arphdr->sender_protaddr = to_beu32(kdb_ip);

		kassert(kdb_nic != nil);
		[kdb_nic debuggerTransmit:p];

		return 1;
	}

	return 0;
}

int
do_gdb_packet(struct pbuf *p)
{
	struct ethhdr *ethhdr = (struct ethhdr *)p->payload;
	struct iphdr *iphdr = (struct iphdr *)(p->payload +
	    sizeof(struct ethhdr));

	if (iphdr->protocol == IP_PROTO_UDP) {
		struct udphdr *udphdr = (struct udphdr *)((uint8_t *)iphdr +
		    ((iphdr->ver_ihl & 0x0F) * 4));

		if (from_beu16(udphdr->dest_port) == kdb_port) {
			uint8_t *data = (uint8_t *)udphdr +
			    sizeof(struct udphdr);
			int data_len = from_beu16(udphdr->len) -
			    sizeof(struct udphdr);

			memcpy(received_cmd, data, data_len);
			received_cmd[data_len] = '\0';

			kdb_remote_ip = from_beu32(iphdr->src_addr);
			kdb_remote_port = from_beu16(udphdr->src_port);
			memcpy(kdb_remote_mac, ethhdr->src,
			    sizeof(dk_mac_address_t));

			return 1;
		}
	}

	return 0;
}

uint16_t
ip_checksum(void *vdata, size_t length)
{
	char *data = (char *)vdata;
	uint32_t acc = 0xffff;

	for (size_t i = 0; i + 1 < length; i += 2) {
		beu16_t word;
		memcpy(&word, data + i, 2);
		acc += from_beu16(word);
		if (acc > 0xffff)
			acc -= 0xffff;
	}
	if (length & 1) {
		beu16_t word = to_beu16(0);
		memcpy(&word, data + length - 1, 1);
		acc += from_beu16(word);
		if (acc > 0xffff)
			acc -= 0xffff;
	}
	return ~acc;
}

uint16_t
udp_checksum(struct iphdr *iphdr, struct udphdr *udphdr, void *data, size_t len)
{
	struct {
		beu32_t src_addr;
		beu32_t dst_addr;
		uint8_t zero;
		uint8_t protocol;
		beu16_t udp_len;
	} pseudhdr;
	uint32_t acc = 0;
	beu16_t *ptr;

	pseudhdr.src_addr = iphdr->src_addr;
	pseudhdr.dst_addr = iphdr->dst_addr;
	pseudhdr.zero = 0;
	pseudhdr.protocol = iphdr->protocol;
	pseudhdr.udp_len = udphdr->len;

	ptr = (beu16_t *)&pseudhdr;
	for (size_t i = 0; i < sizeof(pseudhdr) / 2; i++)
		acc += from_beu16(*ptr++);

	ptr = (beu16_t *)udphdr;
	for (size_t i = 0; i < sizeof(struct udphdr) / 2; i++)
		acc += from_beu16(*ptr++);

	ptr = (beu16_t *)data;
	for (size_t i = 0; i < (len + 1) / 2; i++)
		acc += from_beu16(*ptr++);

	while (acc > 0xffff)
		acc = (acc >> 16) + (acc & 0xffff);

	return ~acc;
}

void
send_gdb_response(DKNIC *nic, void *gdb_response, int response_len,
    uint32_t dest_ip, uint16_t dst_port, dk_mac_address_t dest_mac)
{
	size_t full_len = sizeof(struct ethhdr) + sizeof(struct iphdr) +
	    sizeof(struct udphdr) + response_len;
	uint8_t *ptr = tx_buf;
	struct pbuf_custom p;
	struct iphdr *iphdr;
	struct udphdr *udphdr;

	pbuf_alloced_custom(PBUF_RAW, full_len, PBUF_REF, &p, ptr, full_len);

	struct ethhdr *ethhdr = (struct ethhdr *)ptr;
	memcpy(ethhdr->dst, dest_mac, sizeof(dk_mac_address_t));
	memcpy(ethhdr->src, kdb_mac, sizeof(dk_mac_address_t));
	ethhdr->type = to_beu16(ETHERTYPE_IP);
	ptr += sizeof(struct ethhdr);

	iphdr = (struct iphdr *)ptr;
	iphdr->ver_ihl = 0x45;
	iphdr->tos = 0;
	iphdr->tot_len = to_beu16(
	    sizeof(struct iphdr) + sizeof(struct udphdr) + response_len);
	iphdr->id = to_beu16(0);
	iphdr->frag_off = to_beu16(0);
	iphdr->ttl = 64;
	iphdr->protocol = IP_PROTO_UDP;
	iphdr->check = to_beu16(0);
	iphdr->src_addr = to_beu32(kdb_ip);
	iphdr->dst_addr = to_beu32(dest_ip);
	iphdr->check = to_beu16(
	    ip_checksum((void *)iphdr, sizeof(struct iphdr)));
	ptr += sizeof(struct iphdr);

	udphdr = (struct udphdr *)ptr;
	udphdr->src_port = to_beu16(kdb_port);
	udphdr->dest_port = to_beu16(dst_port);
	udphdr->len = to_beu16(sizeof(struct udphdr) + response_len);
	udphdr->check = to_beu16(0);
	ptr += sizeof(struct udphdr);

	memcpy(ptr, gdb_response, response_len);

	udphdr->check = to_beu16(
	    udp_checksum(iphdr, udphdr, gdb_response, response_len));

	[kdb_nic debuggerTransmit:&p.pbuf];
}

void
send_plus(void)
{
	send_gdb_response(kdb_nic, "+", 1, kdb_remote_ip, kdb_remote_port,
	    kdb_remote_mac);
}

static unsigned char
gdb_checksum(const char *s)
{
	unsigned char checksum = 0;
	while (*s) {
		checksum += *s++;
	}
	return checksum;
}

int
send_packet(const char *format, ...)
{
	va_list args;
	int n;
	unsigned char checksum;
	static char resp_buffer[1400];

	va_start(args, format);
	n = npf_vsnprintf(resp_buffer + 1, 1400 - 5, format, args);
	va_end(args);
	kassert(n >= 0 && n < 1395);

	resp_buffer[0] = '$';
	checksum = gdb_checksum(resp_buffer + 1);
	npf_snprintf(resp_buffer + n + 1, 3, "#%02x", checksum);

	resp_buffer[n + 4] = '\0';

	send_gdb_response(kdb_nic, resp_buffer, n + 4, kdb_remote_ip,
	    kdb_remote_port, kdb_remote_mac);

	kprintf(" -> Sending <%s>\n", resp_buffer);

	return 0;
}

static void
process_query(char *cmd)
{
	char *args = strchr(cmd, ':');

	if (args != NULL)
		*args++ = '\0';

	if (!strcmp(cmd, "Supported")) {
		send_packet("PacketSize=1400;");
	} else if (!strcmp(cmd, "Attached")) {
		send_packet("1");
	} else {
		send_packet("");
	}
}


static void
send_registers(void)
{
#ifdef __amd64__
	struct __attribute__((packed)) gdb_regs {
		uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15, rip;
		uint32_t eflags, cs, ss, ds, es, fs;
	} regs;

#define COPY_REG(REG) regs.REG = kdb_saved_frame->REG
	COPY_REG(rax);
	COPY_REG(rbx);
	COPY_REG(rcx);
	COPY_REG(rdx);
	COPY_REG(rsi);
	COPY_REG(rdi);
	COPY_REG(rbp);
	COPY_REG(rsp);
	COPY_REG(r8);
	COPY_REG(r9);
	COPY_REG(r10);
	COPY_REG(r11);
	COPY_REG(r12);
	COPY_REG(r13);
	COPY_REG(r14);
	COPY_REG(r15);
	COPY_REG(rip);
	COPY_REG(cs);
	regs.ss = regs.ds = regs.es = regs.fs = kdb_saved_frame->ss;
	regs.eflags = kdb_saved_frame->rflags & 0xffffffff;

#define HEX_64 "%02X" "%02X" "%02X" "%02X" "%02X" "%02X" "%02X" "%02X"
#define HEX_32 "%02X" "%02X" "%02X" "%02X"

#define HEX_BYTE(val, byte_idx) (((val) >> ((byte_idx) * 8)) & 0xFF)

#define HEX_64_ARGS(val) \
	HEX_BYTE(val, 0), HEX_BYTE(val, 1), HEX_BYTE(val, 2), HEX_BYTE(val, 3),\
	HEX_BYTE(val, 4), HEX_BYTE(val, 5), HEX_BYTE(val, 6), HEX_BYTE(val, 7)

#define HEX_32_ARGS(val) \
	HEX_BYTE(val, 0), HEX_BYTE(val, 1), HEX_BYTE(val, 2), HEX_BYTE(val, 3)

	send_packet(HEX_64 HEX_64 HEX_64 HEX_64 HEX_64 HEX_64 HEX_64 HEX_64
	    HEX_64 HEX_64 HEX_64 HEX_64 HEX_64 HEX_64 HEX_64 HEX_64
	    HEX_64 HEX_32 HEX_32 HEX_32 HEX_32 HEX_32 HEX_32,
	    HEX_64_ARGS(regs.rax), HEX_64_ARGS(regs.rbx), HEX_64_ARGS(regs.rcx),
	    HEX_64_ARGS(regs.rdx), HEX_64_ARGS(regs.rsi), HEX_64_ARGS(regs.rdi),
	    HEX_64_ARGS(regs.rbp), HEX_64_ARGS(regs.rsp), HEX_64_ARGS(regs.r8),
	    HEX_64_ARGS(regs.r9), HEX_64_ARGS(regs.r10), HEX_64_ARGS(regs.r11),
	    HEX_64_ARGS(regs.r12), HEX_64_ARGS(regs.r13), HEX_64_ARGS(regs.r14),
	    HEX_64_ARGS(regs.r15), HEX_64_ARGS(regs.rip),
	    HEX_32_ARGS(regs.eflags), HEX_32_ARGS(regs.cs),
	    HEX_32_ARGS(regs.ss), HEX_32_ARGS(regs.ds), HEX_32_ARGS(regs.es),
	    HEX_32_ARGS(regs.fs));
#endif
}

static void
process_command(char *cmd)
{
	switch (cmd[0]) {
	case 0x03:
		send_packet("T%02Xthread:%X;", 5, 1);
		return;

	case '$':
		break;

	case '-':
		kprintf("missed something?\n");
		/* fallthrough */
	case '+':
		return;

	default:
		kprintf("malformed packet?\n");
		return;
	}

	cmd++;

	switch (cmd[0]) {
	case '?':
		send_packet("T%02Xthread:%X;", 5, 1);
		break;

	case 'c':
		kfatal("Continue\n");
		break;

	case 'g': /* send registers */
		send_registers();
		break;

	case 'H': /* select thread */
		send_packet("OK");
		break;

	case 'q':
		return process_query(cmd + 1);

	case 'T':
		send_packet("OK");
		break;
	default:
		send_packet("");
	}
}

int
kdbudp_check_packet(void)
{
	ipl_t ipl = splraise(kIPLHigh);
	if (do_arp_packet(&kdb_udp_rx_pbuf)) {
		splx(ipl);
		return 0;
	} else if (do_gdb_packet(&kdb_udp_rx_pbuf)) {
#ifdef __amd64__
		asm("int $255");
#endif
		splx(ipl);
		return 1;
	} else {
		return 0;
	}
}

int
kdb_enter(md_intr_frame_t *frame)
{
	kdb_saved_frame = frame;

	/* if we are here, we have a command to deal with */
	if (received_cmd[0] != '+')
		kprintf("Received <%s>\n", received_cmd);
	send_plus();
	process_command(received_cmd);

	while (true) {
		[kdb_nic debuggerPoll];

		if (do_arp_packet(&kdb_udp_rx_pbuf)) {
			kprintf("did arp\n");
		} else if (do_gdb_packet(&kdb_udp_rx_pbuf)) {
			if (received_cmd[0] != '+')
				kprintf("Received <%s>\n", received_cmd);
			send_plus();
			process_command(received_cmd);
		}
	}
}
#endif
