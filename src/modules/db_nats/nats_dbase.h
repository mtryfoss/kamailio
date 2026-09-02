/*
 * Copyright (C) 2026 Morten Tryfoss
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _DB_NATS_DBASE_H_
#define _DB_NATS_DBASE_H_

#include "../../lib/srdb1/db.h"
#include "../../lib/srdb1/db_con.h"
#include "../../lib/srdb1/db_res.h"
#include "../../lib/srdb1/db_key.h"
#include "../../lib/srdb1/db_op.h"
#include "../../lib/srdb1/db_val.h"
#include "../../core/str.h"

db1_con_t *db_nats_init(const str *_url);
db1_con_t *db_nats_init2(const str *_url, db_pooling_t _pooling);
void db_nats_close(db1_con_t *_h);

int db_nats_use_table(db1_con_t *_h, const str *_t);
int db_nats_free_result(db1_con_t *_h, db1_res_t *_r);

int db_nats_query(const db1_con_t *_h, const db_key_t *_k, const db_op_t *_op,
		const db_val_t *_v, const db_key_t *_c, const int _n, const int _nc,
		const db_key_t _o, db1_res_t **_r);

int db_nats_fetch_result(const db1_con_t *_h, db1_res_t **_r, const int _nrows);

int db_nats_raw_query(const db1_con_t *_h, const str *_s, db1_res_t **_r);

int db_nats_insert(const db1_con_t *_h, const db_key_t *_k, const db_val_t *_v,
		const int _n);

int db_nats_delete(const db1_con_t *_h, const db_key_t *_k, const db_op_t *_o,
		const db_val_t *_v, const int _n);

int db_nats_update(const db1_con_t *_h, const db_key_t *_k, const db_op_t *_o,
		const db_val_t *_v, const db_key_t *_uk, const db_val_t *_uv,
		const int _n, const int _un);

int db_nats_replace(const db1_con_t *_h, const db_key_t *_k, const db_val_t *_v,
		const int _n, const int _un, const int _m);

int db_nats_insert_update(const db1_con_t *_h, const db_key_t *_k,
		const db_val_t *_v, const int _n);

int db_nats_affected_rows(const db1_con_t *_h);

#endif
