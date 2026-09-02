/*
 * Copyright (C) 2026 Morten Tryfoss
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _DB_NATS_PROTO_H_
#define _DB_NATS_PROTO_H_

#include "../../lib/srdb1/db.h"
#include "../../lib/srdb1/db_val.h"
#include "../../lib/srdb1/db_res.h"
#include "../../core/str.h"

#include "nats_connection.h"

/*! \brief Wire name for a db_val type, e.g. DB1_STRING -> "string". */
const char *db_nats_type2str(db_type_t t);

/*! \brief Inverse of db_nats_type2str. Returns DB1_UNKNOWN when unmatched. */
db_type_t db_nats_str2type(const char *s);

/*
 * Request builders. Each fills payload with a NUL-terminated pkg buffer that
 * the caller must pkg_free().
 */

int db_nats_enc_query(const db1_con_t *_h, const db_key_t *_k,
		const db_op_t *_op, const db_val_t *_v, const db_key_t *_c,
		const int _n, const int _nc, const db_key_t _o, int fetch,
		str *payload);

int db_nats_enc_insert(const db1_con_t *_h, const char *op, const db_key_t *_k,
		const db_val_t *_v, const int _n, const int _un, str *payload);

int db_nats_enc_update(const db1_con_t *_h, const db_key_t *_k,
		const db_op_t *_o, const db_val_t *_v, const db_key_t *_uk,
		const db_val_t *_uv, const int _n, const int _un, str *payload);

int db_nats_enc_delete(const db1_con_t *_h, const db_key_t *_k,
		const db_op_t *_o, const db_val_t *_v, const int _n, str *payload);

int db_nats_enc_raw(const db1_con_t *_h, const str *sql, str *payload);

int db_nats_enc_fetch(const db1_con_t *_h, const int nrows, str *payload);

/*!
 * \brief Decode a reply.
 *
 * Always consumes the status, affected-row count and cursor id. Then:
 *  - _r == NULL        : nothing further (caller wanted no result)
 *  - _r != NULL, *_r==0: allocate a fresh result with columns and rows
 *  - _r != NULL, *_r!=0: refill rows only, reusing the existing columns
 *
 * \return 0 on success, negative on error
 */
int db_nats_dec_reply(const db1_con_t *_h, str *reply, db1_res_t **_r);

#endif
