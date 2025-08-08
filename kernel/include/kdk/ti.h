/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Feb 24 2025.
 */
/*!
 * @file ti.h
 * @brief Transport Interface definitions.
 */

#ifndef KRX_TI_H
#define KRX_TI_H

#include <sys/socket.h>

#include <stdint.h>

struct T_opthdr {
	uint32_t	len;	/* total length of option (incl. T_opthdr) */
	uint32_t	level;	/* protocol level	*/
	uint32_t	name;	/* option name		*/
	uint32_t	status;	/* status value		*/
};

#define TI_OPT_ALIGN(len) (ROUNDUP(len, sizeof(uintptr_t)))

/* size_t TI_OPT_LEN(size_t len) - calc value that to be put in len */
#define TI_OPT_LEN(s) (sizeof(struct T_opthdr) + (s))
/* size_t TI_OPT_SPACE(size_t len) - calc total length incl. padding of option */
#define TI_OPT_SPACE(s) (sizeof(struct T_opthdr) + TI_OPT_ALIGN(s))

#define __TI_OPT_NEXT(opt) ((char *)(opt) + TI_OPT_ALIGN((opt)->len))
#define __TI_OPTBUF_LIMIT(buf, buflen) ((char *)(buf) + (buflen))

#define TI_OPT_FIRSTHDR(buf, buflen) \
	((buflen) < sizeof(struct T_opthdr) ? \
	    (struct T_opthdr *)0 : (struct T_opthdr *)(buf))

#define TI_OPT_NXTHDR(buf, buflen, opt) \
	(((opt)->len < sizeof(struct T_opthdr) || \
	    (ptrdiff_t)(sizeof(struct T_opthdr) + TI_OPT_ALIGN((opt)->len)) \
		>= (__TI_OPTBUF_LIMIT((buf), (buflen)) - (char *)(opt))) \
	    ? (struct T_opthdr *)0 : (struct T_opthdr *)__TI_OPT_NEXT(opt))

#define TI_OPT_DATA_LEN(opt) ((opt)->len - sizeof(struct T_opthdr))
#define TI_OPT_DATA(opt) ((char *)(opt) + sizeof(struct T_opthdr))

enum T_prim {
	T_CONN_REQ = 0,
	T_CONN_RES = 1,
	T_DISCON_REQ = 2,
	T_DATA_REQ = 3,
	T_BIND_REQ = 6,
	T_UNITDATA_REQ = 8,
	T_ORDREL_REQ = 10,
	T_CONN_IND = 11,
	T_CONN_CON = 12,
	T_DISCON_IND = 13,
	T_DATA_IND = 14,
	T_BIND_ACK = 17,
	T_ERROR_ACK = 18,
	T_OK_ACK = 19,
	T_UNITDATA_IND = 20,
	T_ORDREL_IND = 23,
	T_OPTDATA_REQ = 24,
	T_OPTDATA_IND = 26,
};

#pragma mark - User-originated

/*
 * @brief Bind Request
 *
 * User-originated. Binds a local address to a socket.
 */
struct T_bind_req {
	enum T_prim PRIM_type;
	struct sockaddr_storage ADDR;
	unsigned long CONIND_number;
};

/*
 * @brief Connect Request
 *
 * User-originated. Initiates a connection request to a remote peer.
 */
struct T_conn_req {
	enum T_prim PRIM_type;
	struct sockaddr_storage DEST;
};

/*
 * @brief Connect Response
 *
 * User-originated. Accept a connection request.
 */
struct T_conn_res {
	enum T_prim PRIM_type;
	void *ACCEPTOR_id;
	uintptr_t SEQ_number;
};

/*
 * @brief Disconnect Request.
 *
 * User-originated. Request to abortively disconnect an existing connection or
 * deny a request for a connection.
 */
struct T_discon_req {
	enum T_prim PRIM_type;
	uintptr_t SEQ_number;
};

/*!
 * @brief Orderly Release Request
 *
 * User-originated. Request to release a connection in an orderly manner: no
 * further data will be sent by the user.
 */
struct T_ordrel_req {
	enum T_prim PRIM_type;
};

/*!
 * @brief Data Request
 *
 * Headed with an M_PROTO block containing this structure.
 * Followed by M_DATA blocks.
 */
struct T_data_req {
	enum T_prim PRIM_type;
	uint32_t MORE_flag;
};

/*!
 * @brief Data Request with Options
 *
 * Headed with an M_PROTO block containing this structure then the options.
 * Followed by M_DATA blocks.
 */
struct T_optdata_req {
	enum T_prim PRIM_type;
	uint32_t DATA_flag;
	uint32_t OPT_length;
	uint32_t OPT_offset;
};

/*
 * unitdata_req | dest addr [... align to word ] | opt 1 [... align to word ]
 */

/*!
 * @brief Unit Data Request
 *
 * Headed with an M_PROTO block containing this structure then the dest addr and
 * options.
 * Followed by M_DATA blocks.
 */
struct T_unitdata_req {
	enum T_prim PRIM_type;
	uint32_t DEST_length;
	uint32_t DEST_offset;
	uint32_t OPT_length;
	uint32_t OPT_offset;
};

#pragma mark - Provider-originated

/*
 * Bind Acknowledgement
 *
 * Provider-originated. Indicates success of a bind request.
 */
struct T_bind_ack {
	enum T_prim PRIM_type;
	struct sockaddr_storage ADDR;
	unsigned long CONIND_number;
};

/*
 * Connect Confirm
 *
 * Provider-originated. Indicates success of a connection request.
 */
struct T_conn_con {
	enum T_prim PRIM_type;
};

/*
 * Connect Indication
 *
 * Provider-originated. Indicates a connection request from a remote peer.
 */
struct T_conn_ind {
	enum T_prim PRIM_type;
	struct sockaddr_storage DEST;
};

/*
 * Success Acknowledgement
 *
 * Provider-originated. Indicates success of a request.
 */
struct T_ok_ack {
	enum T_prim PRIM_type;
	enum T_prim CORRECT_prim;
};

/*
 * Error Acknowledgement
 *
 * Provider-originated. Indicates failure of a request.
 */
struct T_error_ack {
	enum T_prim PRIM_type;
	enum T_prim ERROR_prim;
	long UNIX_error;
};

/*
 * Disconnect Indication
 *
 * Provider-originated. Indicates an abortive disconnection of an existing
 * connection or the denial of a connection attempt by a remote peer.
 */
struct T_discon_ind {
	enum T_prim PRIM_type;
	uintptr_t DISCON_reason;
	uintptr_t SEQ_number;
};

/*
 * Orderly Release Indication
 *
 * Provider-originated. Indicates an orderly release of a connection by remote
 * peer: no further data will be sent by the remote peer.
 */
struct T_ordrel_ind {
	enum T_prim PRIM_type;
};

union T_primitives {
	enum T_prim type;

	struct T_bind_req bind_req;
	struct T_conn_req conn_req;
	struct T_conn_res conn_res;
	struct T_discon_req discon_req;
	struct T_ordrel_req ordrel_req;
	struct T_data_req data_req;
	struct T_optdata_req optdata_req;
	struct T_unitdata_req unitdata_req;

	struct T_bind_ack bind_ack;
	struct T_conn_con conn_con;
	struct T_conn_ind conn_ind;
	struct T_ok_ack ok_ack;
	struct T_error_ack error_ack;
	struct T_ordrel_ind ordrel_ind;
};

#endif /* KRX_TI_H */
