/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Thu Feb 26 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file tihdr.h
 * @brief Transport Interface definitions.
 */

#ifndef ECX_SYS_TIHDR_H
#define ECX_SYS_TIHDR_H

#include <sys/socket.h>

/*
 * sockaddr structure used (kernel-side) by the Unix transport.
 */
struct sockaddr_ux {
	sa_family_t sun_family; /* AF_UX */
	struct queue *sux_rq;	/* associated read queue */
	char sun_path[108];	/* pathname */
};
#define AF_UX 220

enum T_prim {
	T_CONN_REQ = 0,		/* connection request */
	T_CONN_RES = 1,		/* connection response*/
	T_BIND_REQ = 6,		/* bind request */
	T_UNITDATA_REQ = 8,	/* unit data request */
	T_OPTMGMT_REQ = 9,
	T_ORDREL_REQ = 10,	/* orderly-release request */

	T_CONN_IND = 11,	/* connection indication */
	T_CONN_CON = 12,	/* connection confirmation */
	T_DISCON_IND = 13,	/* disconnection indication */

	T_BIND_ACK = 17,	/* bind acknowledgement */
	T_ERROR_ACK = 18,	/* error acknowledgement */
	T_OK_ACK = 19,		/* success acknowledge */
	T_UNITDATA_IND = 20,	/* unit data indication */
	T_OPTMGMT_ACK = 22,
	T_ORDREL_IND = 23,	/* orderly-release indication */

	T_ADDR_REQ = 24,	/* get local/peer address request */
	T_ADDR_ACK = 25,	/* get local/peer address acknowledgment */

};

/*
 * @brief Bind Request
 *
 * User-originated. Binds a local address to a socket.
 */
struct T_bind_req {
	enum T_prim PRIM_type;
	int ADDR_length;
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
	int DEST_length;
	struct sockaddr_storage DEST;
	size_t padding;
};

/*
 * @brief Connect Response
 *
 * User-originated. Response to an incoming connection request.
 */
struct T_conn_res {
	enum T_prim PRIM_type;
	size_t ACCEPTOR_id;
	size_t SEQ_number;
};

/*
 * @brief Unit Data Request
 *
 * User-originated. Transmits a message to a peer without establishing a
 * connection.
 */
struct T_unitdata_req {
	enum T_prim PRIM_type;
	int DEST_length;
	struct sockaddr_storage DEST;
};

/*
 * @brief Connect Indication
 *
 * Provider-originated. Indicates an incoming connection request.
 */
struct T_conn_ind {
	enum T_prim PRIM_type;
	int SRC_length;
	struct sockaddr_storage SRC;
	size_t SEQ_number;
};

/*
 * @brief Connect Request
 *
 * User-originated. Initiates a connection request to a remote peer.
 */
struct T_conn_con {
	enum T_prim PRIM_type;
	int RES_length;
	struct sockaddr_storage RES;
};


/* @brief Address Request
 *
 * User-originated. Requests the local and peer address of a socket.
 */
struct T_addr_req {
	enum T_prim	PRIM_type;
};

/*
 * @brief Disconnect Indication
 *
 * Provider-originated. Indicates an abortive disconnection of an existing
 * connection or the denial of a connection attempt by a remote peer.
 */
struct T_discon_ind {
	enum T_prim PRIM_type;
	size_t DISCON_reason;
	size_t SEQ_number;
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

/*
 * @brief Options Management Request
 *
 * User-originated. Sets or gets socket options.
 */
struct T_optmgmt_req {
	enum T_prim  PRIM_type;
	size_t OPT_length;
	size_t OPT_offset;
	size_t MGMT_flags;
};

/*
 * provider-originated
 */

/*
 * @brief Bind Acknowledgement
 *
 * Provider-originated. Acknowledges a bind request.
 */
struct T_bind_ack {
	enum T_prim PRIM_type;
	int ADDR_length;
	struct sockaddr_storage ADDR;
	unsigned long CONIND_number;
};

/*
 * @brief Address Acknowledgement
 *
 * Provider-originated. Acknowledges an address request, returning the local
 * and peer address of the socket.
 */
struct T_addr_ack {
	enum T_prim PRIM_type;
	int LOCADDR_length;
	struct sockaddr_storage LOCADDR;
	int REMADDR_length;
	struct sockaddr_storage REMADDR;
};

/*
 * @brief Error Acknowledgement
 *
 * Provider-originated. Indicates an error in response to a user request.
 */
struct T_error_ack {
	enum T_prim PRIM_type;
	enum T_prim ERROR_prim;
	int UNIX_error;
};

/*
 * @brief OK Acknowledgement
 *
 * Provider-originated. Indicates successful completion of a user request.
 */
struct T_ok_ack {
	enum T_prim PRIM_type;
	enum T_prim CORRECT_prim;
};

/*
 * @brief Unit Data Indication
 *
 * Provider-originated. Indicates the arrival of a message sent by a peer via
 * a unit data request.
 */
struct T_unitdata_ind {
	enum T_prim PRIM_type;
	int SRC_length;
	struct sockaddr_storage SRC;
};

/*
 * @brief Options Management Acknowledgement
 *
 * Provider-originated. Acknowledges an options management request.
 */
struct T_optmgmt_ack {
	enum T_prim  PRIM_type;
	size_t OPT_length;
	size_t OPT_offset;
	size_t MGMT_flags;
};


/* @brief Union of all TI primitives */
union T_primitives {
	enum T_prim type;
	struct T_bind_req bind_req;
	struct T_conn_req conn_req;
	struct T_conn_res conn_res;
	struct T_addr_req addr_req;
	struct T_unitdata_req unitdata_req;
	struct T_optmgmt_req optmgmt_req;
	struct T_conn_ind conn_ind;
	struct T_conn_con conn_con;
	struct T_addr_ack addr_ack;
	struct T_bind_ack bind_ack;
	struct T_error_ack error_ack;
	struct T_ok_ack ok_ack;
	struct T_unitdata_ind unitdata_ind;
	struct T_optmgmt_ack optmgmt_ack;
};

#define T_NEGOTIATE 0x004
#define T_CHECK 0x008

struct opthdr {
	size_t level;
	size_t name;
	size_t len;
};

#define OPTLEN(x) ((((x) + sizeof(size_t) - 1) / \
    sizeof(size_t)) * sizeof(size_t))
#define OPTVAL(opt) ((char *)(opt + 1))


#endif /* ECX_SYS_TIHDR_H */
