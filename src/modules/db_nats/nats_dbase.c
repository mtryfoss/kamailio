/*
 * Copyright (C) 2026 Morten Tryfoss
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "../../core/dprint.h"
#include "../../core/mem/mem.h"
#include "../../lib/srdb1/db.h"
#include "../../lib/srdb1/db_res.h"

#include "db_nats_mod.h"
#include "nats_connection.h"
#include "nats_proto.h"
#include "nats_dbase.h"

db1_con_t *db_nats_init(const str *_url)
{
	return db_do_init(_url, (void *)db_nats_new_connection);
}

db1_con_t *db_nats_init2(const str *_url, db_pooling_t _pooling)
{
	return db_do_init2(_url, (void *)db_nats_new_connection, _pooling);
}

void db_nats_close(db1_con_t *_h)
{
	db_do_close(_h, db_nats_free_connection);
}

int db_nats_use_table(db1_con_t *_h, const str *_t)
{
	return db_use_table(_h, _t);
}

int db_nats_free_result(db1_con_t *_h, db1_res_t *_r)
{
	if(_r == NULL)
		return 0;

	/* the caller is done with this result set, so release any server-side
	 * cursor it was paging through - the API has no explicit close */
	if(_h != NULL && CON_NATS(_h) != NULL)
		db_nats_cursor_close(CON_NATS(_h));

	db_free_result(_r);
	return 0;
}

int db_nats_affected_rows(const db1_con_t *_h)
{
	if(_h == NULL)
		return -1;
	return CON_NATS(_h)->affected_rows;
}

/*
 * ---------------------------------------------------------------------------
 * session-state SQL detection
 * ---------------------------------------------------------------------------
 * These statements only mean anything on a connection that persists across
 * calls. Over stateless request/reply each one could land on a different
 * backend and a different MySQL connection, so they are never put on the wire.
 */

/*! \brief Case-insensitive prefix test that also demands a word boundary. */
static int nats_starts_with(const char *p, int len, const char *kw)
{
	int kl = strlen(kw);

	if(len < kl)
		return 0;
	if(strncasecmp(p, kw, kl) != 0)
		return 0;
	/* "COMMIT" must not match "COMMITTED" */
	if(len > kl && (isalnum((unsigned char)p[kl]) || p[kl] == '_'))
		return 0;
	return 1;
}

/*! \brief Bounded case-insensitive substring search. */
static int nats_contains(const char *p, int len, const char *needle)
{
	int nl = strlen(needle);
	int i;

	for(i = 0; i + nl <= len; i++) {
		if(strncasecmp(p + i, needle, nl) == 0)
			return 1;
	}
	return 0;
}

static const char *nats_session_keywords[] = {"BEGIN", "START", "COMMIT",
		"ROLLBACK", "SAVEPOINT", "RELEASE", "LOCK", "UNLOCK", NULL};

/*!
 * \brief Decide whether a raw statement carries session state.
 * \return 1 when it must not be forwarded, 0 otherwise
 */
static int nats_is_session_sql(const str *s)
{
	const char *p = s->s;
	int len = s->len;
	int i;

	/* skip leading whitespace and both comment styles */
	while(len > 0) {
		if(isspace((unsigned char)*p)) {
			p++;
			len--;
		} else if(len > 1 && p[0] == '-' && p[1] == '-') {
			while(len > 0 && *p != '\n') {
				p++;
				len--;
			}
		} else if(len > 1 && p[0] == '/' && p[1] == '*') {
			p += 2;
			len -= 2;
			while(len > 1 && !(p[0] == '*' && p[1] == '/')) {
				p++;
				len--;
			}
			if(len > 1) {
				p += 2;
				len -= 2;
			}
		} else {
			break;
		}
	}

	if(len <= 0)
		return 0;

	for(i = 0; nats_session_keywords[i] != NULL; i++) {
		if(nats_starts_with(p, len, nats_session_keywords[i]))
			return 1;
	}

	/* SET is only a problem for the session-scoped forms */
	if(nats_starts_with(p, len, "SET")) {
		if(nats_contains(p, len, "AUTOCOMMIT")
				|| nats_contains(p, len, "SESSION")
				|| nats_contains(p, len, "TRANSACTION"))
			return 1;
	}

	/* advisory locks are bound to a connection */
	if(nats_contains(p, len, "GET_LOCK(")
			|| nats_contains(p, len, "RELEASE_LOCK("))
		return 1;

	/* row locks only hold inside a transaction */
	if(nats_contains(p, len, "FOR UPDATE"))
		return 1;

	return 0;
}

/*! \brief Hand back an empty result so the caller's free_result stays valid. */
static int nats_empty_result(db1_res_t **_r)
{
	db1_res_t *r;

	if(_r == NULL)
		return 0;

	r = db_new_result();
	if(r == NULL) {
		LM_ERR("failed to allocate empty result\n");
		return -1;
	}
	RES_COL_N(r) = 0;
	RES_ROW_N(r) = 0;
	RES_ROWS(r) = NULL;
	*_r = r;
	return 0;
}

/*! \brief Shared tail: send a prepared payload and decode the reply. */
static int nats_roundtrip(
		const db1_con_t *_h, str *subject, str *payload, db1_res_t **_r)
{
	str reply;
	int ret;

	ret = db_nats_request(CON_NATS(_h), subject, payload, &reply);
	pkg_free(payload->s);
	if(ret < 0)
		return -1;

	ret = db_nats_dec_reply(_h, &reply, _r);
	pkg_free(reply.s);
	return ret;
}

int db_nats_query(const db1_con_t *_h, const db_key_t *_k, const db_op_t *_op,
		const db_val_t *_v, const db_key_t *_c, const int _n, const int _nc,
		const db_key_t _o, db1_res_t **_r)
{
	str payload;

	if(_h == NULL) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	/* a new statement invalidates whatever cursor was still open */
	db_nats_cursor_close(CON_NATS(_h));

	/* _r == NULL is the srdb1 signal to start a fetch cursor */
	if(db_nats_enc_query(
			   _h, _k, _op, _v, _c, _n, _nc, _o, (_r == NULL), &payload)
			< 0)
		return -1;

	return nats_roundtrip(_h, &CON_NATS(_h)->subj_op, &payload, _r);
}

int db_nats_fetch_result(const db1_con_t *_h, db1_res_t **_r, const int _nrows)
{
	nats_con_t *con;
	str payload;

	if(_h == NULL || _r == NULL || _nrows < 0) {
		LM_ERR("invalid parameter\n");
		return -1;
	}
	con = CON_NATS(_h);

	/* zero rows means "release everything" */
	if(_nrows == 0) {
		if(*_r != NULL) {
			db_nats_free_result((db1_con_t *)_h, *_r);
			*_r = NULL;
		}
		db_nats_cursor_close(con);
		return 0;
	}

	if(con->cursor == NULL) {
		/* cursor exhausted (or never opened) - report an empty page */
		if(*_r != NULL) {
			db_free_rows(*_r);
			RES_ROWS(*_r) = NULL;
			RES_ROW_N(*_r) = 0;
			return 0;
		}
		return nats_empty_result(_r);
	}

	if(db_nats_enc_fetch(_h, _nrows, &payload) < 0)
		return -1;

	/* address the instance that owns the cursor, not the queue group */
	return nats_roundtrip(_h, db_nats_cursor_subj(con), &payload, _r);
}

int db_nats_raw_query(const db1_con_t *_h, const str *_s, db1_res_t **_r)
{
	str payload;

	if(_h == NULL || _s == NULL || _s->s == NULL) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	if(nats_is_session_sql(_s)) {
		if(db_nats_session_sql_mode == DB_NATS_SESSION_IGNORE) {
			LM_WARN("ignoring session-state statement [%.*s] - db_nats is "
					"stateless request/reply, so it carries no meaning\n",
					(_s->len > 64) ? 64 : _s->len, _s->s);
			/* report success with nothing done */
			return nats_empty_result(_r);
		}
		LM_ERR("refusing session-state statement [%.*s]: db_nats cannot "
			   "provide connection affinity over request/reply. Set "
			   "session_sql_mode=\"ignore\" to make this a no-op instead.\n",
				(_s->len > 64) ? 64 : _s->len, _s->s);
		return -1;
	}

	db_nats_cursor_close(CON_NATS(_h));

	if(db_nats_enc_raw(_h, _s, &payload) < 0)
		return -1;

	return nats_roundtrip(_h, &CON_NATS(_h)->subj_raw, &payload, _r);
}

int db_nats_insert(const db1_con_t *_h, const db_key_t *_k, const db_val_t *_v,
		const int _n)
{
	str payload;

	if(_h == NULL || _k == NULL || _v == NULL || _n <= 0) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	if(db_nats_enc_insert(_h, "insert", _k, _v, _n, 0, &payload) < 0)
		return -1;

	return nats_roundtrip(_h, &CON_NATS(_h)->subj_op, &payload, NULL);
}

int db_nats_insert_update(const db1_con_t *_h, const db_key_t *_k,
		const db_val_t *_v, const int _n)
{
	str payload;

	if(_h == NULL || _k == NULL || _v == NULL || _n <= 0) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	if(db_nats_enc_insert(_h, "insert_update", _k, _v, _n, 0, &payload) < 0)
		return -1;

	return nats_roundtrip(_h, &CON_NATS(_h)->subj_op, &payload, NULL);
}

int db_nats_replace(const db1_con_t *_h, const db_key_t *_k, const db_val_t *_v,
		const int _n, const int _un, const int _m)
{
	str payload;

	if(_h == NULL || _k == NULL || _v == NULL || _n <= 0) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	if(db_nats_enc_insert(_h, "replace", _k, _v, _n, _un, &payload) < 0)
		return -1;

	return nats_roundtrip(_h, &CON_NATS(_h)->subj_op, &payload, NULL);
}

int db_nats_update(const db1_con_t *_h, const db_key_t *_k, const db_op_t *_o,
		const db_val_t *_v, const db_key_t *_uk, const db_val_t *_uv,
		const int _n, const int _un)
{
	str payload;

	if(_h == NULL || _uk == NULL || _uv == NULL || _un <= 0) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	if(db_nats_enc_update(_h, _k, _o, _v, _uk, _uv, _n, _un, &payload) < 0)
		return -1;

	return nats_roundtrip(_h, &CON_NATS(_h)->subj_op, &payload, NULL);
}

int db_nats_delete(const db1_con_t *_h, const db_key_t *_k, const db_op_t *_o,
		const db_val_t *_v, const int _n)
{
	str payload;

	if(_h == NULL) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	if(db_nats_enc_delete(_h, _k, _o, _v, _n, &payload) < 0)
		return -1;

	return nats_roundtrip(_h, &CON_NATS(_h)->subj_op, &payload, NULL);
}
