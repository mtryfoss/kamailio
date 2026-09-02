/*
 * Copyright (C) 2026 Morten Tryfoss
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../core/dprint.h"
#include "../../core/mem/mem.h"
#include "../../core/basex.h"
#include "../../core/utils/srjson.h"
#include "../../lib/srdb1/db_ut.h"

#include "db_nats_mod.h"
#include "nats_proto.h"

/* keep srjson allocations inside Kamailio's accounted pkg pool */
static void *nats_json_malloc(size_t sz)
{
	return pkg_malloc(sz);
}

static void nats_json_free(void *p)
{
	pkg_free(p);
}

static srjson_Hooks nats_json_hooks = {nats_json_malloc, nats_json_free};

static struct
{
	db_type_t t;
	const char *n;
} nats_type_map[] = {{DB1_INT, "int"}, {DB1_UINT, "uint"},
		{DB1_BIGINT, "bigint"}, {DB1_UBIGINT, "ubigint"},
		{DB1_DOUBLE, "double"}, {DB1_STRING, "string"}, {DB1_STR, "str"},
		{DB1_DATETIME, "datetime"}, {DB1_BLOB, "blob"}, {DB1_BITMAP, "bitmap"},
		{DB1_UNKNOWN, NULL}};

const char *db_nats_type2str(db_type_t t)
{
	int i;
	for(i = 0; nats_type_map[i].n != NULL; i++) {
		if(nats_type_map[i].t == t)
			return nats_type_map[i].n;
	}
	return "unknown";
}

db_type_t db_nats_str2type(const char *s)
{
	int i;
	if(s == NULL)
		return DB1_UNKNOWN;
	for(i = 0; nats_type_map[i].n != NULL; i++) {
		if(strcmp(nats_type_map[i].n, s) == 0)
			return nats_type_map[i].t;
	}
	return DB1_UNKNOWN;
}

/*!
 * \brief Build the {"t":..,"d":..} object for one value.
 *
 * Every datum travels as a JSON string, never a JSON number: srjson stores
 * numbers as double, so 64-bit integers would silently lose precision above
 * 2^53. DATETIME travels as unix epoch seconds rather than a SQL datetime
 * string, which keeps the encoding symmetric and free of local-time ambiguity.
 */
static srjson_t *nats_val_obj(srjson_doc_t *jd, const db_val_t *v)
{
	srjson_t *o;
	char buf[64];
	int n;
	char *eb;
	int bl, el;

	o = srjson_CreateObject(jd);
	if(o == NULL)
		return NULL;

	srjson_AddStringToObject(jd, o, "t", db_nats_type2str(VAL_TYPE(v)));

	if(VAL_NULL(v)) {
		srjson_AddTrueToObject(jd, o, "n");
		return o;
	}

	switch(VAL_TYPE(v)) {
		case DB1_INT:
			n = snprintf(buf, sizeof(buf), "%d", VAL_INT(v));
			break;
		case DB1_UINT:
			n = snprintf(buf, sizeof(buf), "%u", VAL_UINT(v));
			break;
		case DB1_BITMAP:
			n = snprintf(buf, sizeof(buf), "%u", VAL_BITMAP(v));
			break;
		case DB1_BIGINT:
			n = snprintf(buf, sizeof(buf), "%lld", VAL_BIGINT(v));
			break;
		case DB1_UBIGINT:
			n = snprintf(buf, sizeof(buf), "%llu", VAL_UBIGINT(v));
			break;
		case DB1_DOUBLE:
			/* 17 significant digits round-trips an IEEE double exactly */
			n = snprintf(buf, sizeof(buf), "%.17g", VAL_DOUBLE(v));
			break;
		case DB1_DATETIME:
			n = snprintf(buf, sizeof(buf), "%lld", (long long)VAL_TIME(v));
			break;

		case DB1_STRING:
			if(VAL_STRING(v) == NULL) {
				srjson_AddStrToObject(jd, o, "d", "", 0);
			} else {
				srjson_AddStringToObject(jd, o, "d", VAL_STRING(v));
			}
			return o;

		case DB1_STR:
			srjson_AddStrToObject(jd, o, "d", VAL_STR(v).s, VAL_STR(v).len);
			return o;

		case DB1_BLOB:
			bl = VAL_BLOB(v).len;
			if(bl <= 0 || VAL_BLOB(v).s == NULL) {
				srjson_AddStrToObject(jd, o, "d", "", 0);
				return o;
			}
			el = base64_enc_len(bl);
			eb = (char *)pkg_malloc(el);
			if(eb == NULL) {
				PKG_MEM_ERROR;
				return NULL;
			}
			el = base64_enc((unsigned char *)VAL_BLOB(v).s, bl,
					(unsigned char *)eb, el);
			if(el < 0) {
				LM_ERR("failed to base64 encode blob of %d bytes\n", bl);
				pkg_free(eb);
				return NULL;
			}
			srjson_AddStrToObject(jd, o, "d", eb, el);
			pkg_free(eb);
			return o;

		default:
			LM_ERR("unsupported value type %d\n", VAL_TYPE(v));
			return NULL;
	}

	if(n < 0 || n >= (int)sizeof(buf)) {
		LM_ERR("value did not fit the conversion buffer\n");
		return NULL;
	}
	srjson_AddStrToObject(jd, o, "d", buf, n);
	return o;
}

/*! \brief [{"c":name,"o":op,"v":{..}}, ..] - the WHERE terms */
static int nats_add_filter(srjson_doc_t *jd, srjson_t *root, const db_key_t *_k,
		const db_op_t *_op, const db_val_t *_v, const int _n)
{
	srjson_t *arr, *it, *val;
	int i;

	if(_n <= 0)
		return 0;

	arr = srjson_CreateArray(jd);
	if(arr == NULL)
		return -1;

	for(i = 0; i < _n; i++) {
		it = srjson_CreateObject(jd);
		if(it == NULL)
			return -1;
		srjson_AddStrToObject(jd, it, "c", _k[i]->s, _k[i]->len);
		/* a NULL op array means equality for every term */
		srjson_AddStringToObject(jd, it, "o", (_op && _op[i]) ? _op[i] : OP_EQ);
		val = nats_val_obj(jd, &_v[i]);
		if(val == NULL)
			return -1;
		srjson_AddItemToObject(jd, it, "v", val);
		srjson_AddItemToArray(jd, arr, it);
	}

	srjson_AddItemToObject(jd, root, "filter", arr);
	return 0;
}

/*! \brief [{"c":name,"v":{..}}, ..] - used for both "values" and "set" */
static int nats_add_kvlist(srjson_doc_t *jd, srjson_t *root, const char *name,
		const db_key_t *_k, const db_val_t *_v, const int _n)
{
	srjson_t *arr, *it, *val;
	int i;

	arr = srjson_CreateArray(jd);
	if(arr == NULL)
		return -1;

	for(i = 0; i < _n; i++) {
		it = srjson_CreateObject(jd);
		if(it == NULL)
			return -1;
		srjson_AddStrToObject(jd, it, "c", _k[i]->s, _k[i]->len);
		val = nats_val_obj(jd, &_v[i]);
		if(val == NULL)
			return -1;
		srjson_AddItemToObject(jd, it, "v", val);
		srjson_AddItemToArray(jd, arr, it);
	}

	srjson_AddItemToObject(jd, root, name, arr);
	return 0;
}

/*! \brief ["a","b"] - the projection */
static int nats_add_cols(
		srjson_doc_t *jd, srjson_t *root, const db_key_t *_c, const int _nc)
{
	srjson_t *arr;
	int i;

	if(_c == NULL || _nc <= 0)
		return 0;

	arr = srjson_CreateArray(jd);
	if(arr == NULL)
		return -1;
	for(i = 0; i < _nc; i++) {
		srjson_AddItemToArray(
				jd, arr, srjson_CreateStr(jd, _c[i]->s, _c[i]->len));
	}
	srjson_AddItemToObject(jd, root, "cols", arr);
	return 0;
}

/*! \brief Start a request document carrying version, op and table. */
static int nats_doc_start(srjson_doc_t *jd, const db1_con_t *_h, const char *op)
{
	srjson_InitDoc(jd, &nats_json_hooks);
	jd->root = srjson_CreateObject(jd);
	if(jd->root == NULL) {
		LM_ERR("failed to create json root\n");
		return -1;
	}
	srjson_AddNumberToObject(jd, jd->root, "v", DB_NATS_PROTO_VER);
	srjson_AddStringToObject(jd, jd->root, "op", op);
	if(_h != NULL && CON_TABLE(_h) != NULL) {
		srjson_AddStrToObject(
				jd, jd->root, "table", CON_TABLE(_h)->s, CON_TABLE(_h)->len);
	}
	return 0;
}

/*! \brief Serialise and hand the buffer to the caller, who must pkg_free it. */
static int nats_doc_finish(srjson_doc_t *jd, str *payload)
{
	char *s;

	s = srjson_PrintUnformatted(jd, jd->root);
	/* DestroyDoc only releases root, never doc->buf - the string is ours */
	srjson_DestroyDoc(jd);

	if(s == NULL) {
		LM_ERR("failed to serialise request\n");
		return -1;
	}
	payload->s = s;
	payload->len = strlen(s);
	return 0;
}

int db_nats_enc_query(const db1_con_t *_h, const db_key_t *_k,
		const db_op_t *_op, const db_val_t *_v, const db_key_t *_c,
		const int _n, const int _nc, const db_key_t _o, int fetch, str *payload)
{
	srjson_doc_t jd;

	if(nats_doc_start(&jd, _h, "select") < 0)
		return -1;

	if(nats_add_cols(&jd, jd.root, _c, _nc) < 0)
		goto error;
	if(nats_add_filter(&jd, jd.root, _k, _op, _v, _n) < 0)
		goto error;
	if(_o != NULL)
		srjson_AddStrToObject(&jd, jd.root, "order", _o->s, _o->len);
	if(fetch)
		srjson_AddTrueToObject(&jd, jd.root, "fetch");

	return nats_doc_finish(&jd, payload);

error:
	srjson_DestroyDoc(&jd);
	return -1;
}

int db_nats_enc_insert(const db1_con_t *_h, const char *op, const db_key_t *_k,
		const db_val_t *_v, const int _n, const int _un, str *payload)
{
	srjson_doc_t jd;

	if(nats_doc_start(&jd, _h, op) < 0)
		return -1;

	if(nats_add_kvlist(&jd, jd.root, "values", _k, _v, _n) < 0)
		goto error;
	if(_un > 0)
		srjson_AddNumberToObject(&jd, jd.root, "un", _un);

	return nats_doc_finish(&jd, payload);

error:
	srjson_DestroyDoc(&jd);
	return -1;
}

int db_nats_enc_update(const db1_con_t *_h, const db_key_t *_k,
		const db_op_t *_o, const db_val_t *_v, const db_key_t *_uk,
		const db_val_t *_uv, const int _n, const int _un, str *payload)
{
	srjson_doc_t jd;

	if(nats_doc_start(&jd, _h, "update") < 0)
		return -1;

	if(nats_add_kvlist(&jd, jd.root, "set", _uk, _uv, _un) < 0)
		goto error;
	if(nats_add_filter(&jd, jd.root, _k, _o, _v, _n) < 0)
		goto error;

	return nats_doc_finish(&jd, payload);

error:
	srjson_DestroyDoc(&jd);
	return -1;
}

int db_nats_enc_delete(const db1_con_t *_h, const db_key_t *_k,
		const db_op_t *_o, const db_val_t *_v, const int _n, str *payload)
{
	srjson_doc_t jd;

	if(nats_doc_start(&jd, _h, "delete") < 0)
		return -1;

	if(nats_add_filter(&jd, jd.root, _k, _o, _v, _n) < 0)
		goto error;

	return nats_doc_finish(&jd, payload);

error:
	srjson_DestroyDoc(&jd);
	return -1;
}

int db_nats_enc_raw(const db1_con_t *_h, const str *sql, str *payload)
{
	srjson_doc_t jd;

	/* no table: the statement carries its own */
	if(nats_doc_start(&jd, NULL, "raw") < 0)
		return -1;

	srjson_AddStrToObject(&jd, jd.root, "sql", sql->s, sql->len);

	return nats_doc_finish(&jd, payload);
}

int db_nats_enc_fetch(const db1_con_t *_h, const int nrows, str *payload)
{
	srjson_doc_t jd;
	nats_con_t *con = CON_NATS(_h);

	if(con->cursor == NULL) {
		LM_ERR("no open cursor to fetch from\n");
		return -1;
	}

	if(nats_doc_start(&jd, NULL, "fetch") < 0)
		return -1;

	srjson_AddStringToObject(&jd, jd.root, "cursor", con->cursor);
	srjson_AddNumberToObject(&jd, jd.root, "limit", nrows);

	return nats_doc_finish(&jd, payload);
}

/*! \brief Copy the column metadata out of the reply into the result. */
static int nats_dec_cols(srjson_doc_t *jd, srjson_t *cols, db1_res_t *r)
{
	srjson_t *it, *n, *t;
	int ncols, i, len;

	ncols = srjson_GetArraySize(jd, cols);
	if(ncols == 0) {
		RES_COL_N(r) = 0;
		return 0;
	}

	if(db_allocate_columns(r, ncols) != 0) {
		LM_ERR("failed to allocate %d columns\n", ncols);
		return -1;
	}
	/* db_free_columns walks RES_COL_N entries and dereferences RES_NAMES
	 * without a NULL check, so only publish the count once both arrays
	 * exist. They are zeroed, making partially filled slots safe to free. */
	RES_COL_N(r) = ncols;

	/* names are copied out of the json doc, so mark them for freeing */
	RES_COL_FLAGS(r) |= DB1_FCOL_FREE;

	for(i = 0; i < ncols; i++) {
		it = srjson_GetArrayItem(jd, cols, i);
		if(it == NULL)
			goto error;
		n = srjson_GetObjectItem(jd, it, "n");
		t = srjson_GetObjectItem(jd, it, "t");
		if(n == NULL || n->valuestring == NULL) {
			LM_ERR("column %d has no name\n", i);
			goto error;
		}

		RES_NAMES(r)[i] = (str *)pkg_malloc(sizeof(str));
		if(RES_NAMES(r)[i] == NULL) {
			PKG_MEM_ERROR;
			goto error;
		}
		len = strlen(n->valuestring);
		RES_NAMES(r)[i]->s = (char *)pkg_malloc(len + 1);
		if(RES_NAMES(r)[i]->s == NULL) {
			PKG_MEM_ERROR;
			goto error;
		}
		memcpy(RES_NAMES(r)[i]->s, n->valuestring, len);
		RES_NAMES(r)[i]->s[len] = '\0';
		RES_NAMES(r)[i]->len = len;

		RES_TYPES(r)[i] = db_nats_str2type((t != NULL) ? t->valuestring : NULL);
		if(RES_TYPES(r)[i] == DB1_UNKNOWN) {
			LM_WARN("unknown type for column [%s], using string\n",
					n->valuestring);
			RES_TYPES(r)[i] = DB1_STRING;
		}
	}
	return 0;

error:
	/* db_free_columns walks RES_COL_N entries; it tolerates NULL slots */
	return -1;
}

/*! \brief Turn one JSON scalar into a db_val_t of the column's type. */
static int nats_dec_val(srjson_t *e, db_type_t type, db_val_t *val)
{
	char *dec;
	int dl;

	if(e == NULL || e->type == srjson_NULL) {
		/* db_str2val builds a safe NULL value when handed a NULL pointer */
		return db_str2val(type, val, NULL, 0, 0);
	}

	if(e->valuestring == NULL) {
		LM_ERR("row value is not a json string\n");
		return -1;
	}

	if(type == DB1_DATETIME) {
		/* epoch seconds, not a SQL datetime string */
		VAL_TYPE(val) = DB1_DATETIME;
		VAL_NULL(val) = 0;
		VAL_FREE(val) = 0;
		VAL_TIME(val) = (time_t)strtoll(e->valuestring, NULL, 10);
		return 0;
	}

	if(type == DB1_BLOB) {
		dl = strlen(e->valuestring);
		if(dl == 0) {
			VAL_TYPE(val) = DB1_BLOB;
			VAL_NULL(val) = 0;
			VAL_FREE(val) = 0;
			VAL_BLOB(val).s = NULL;
			VAL_BLOB(val).len = 0;
			return 0;
		}
		dec = (char *)pkg_malloc(dl);
		if(dec == NULL) {
			PKG_MEM_ERROR;
			return -1;
		}
		dl = base64_dec(
				(unsigned char *)e->valuestring, dl, (unsigned char *)dec, dl);
		if(dl <= 0) {
			LM_ERR("failed to base64 decode blob\n");
			pkg_free(dec);
			return -1;
		}
		VAL_TYPE(val) = DB1_BLOB;
		VAL_NULL(val) = 0;
		VAL_BLOB(val).s = dec;
		VAL_BLOB(val).len = dl;
		/* db_free_row releases this for us */
		VAL_FREE(val) = 1;
		return 0;
	}

	/* _cpy=1 so the value owns its buffer and survives the json doc;
	 * note this is also what NUL-terminates DB1_STRING values */
	return db_str2val(type, val, e->valuestring, strlen(e->valuestring), 1);
}

/*! \brief Fill RES_ROWS from the reply's "rows" array. */
static int nats_dec_rows(srjson_doc_t *jd, srjson_t *rows, db1_res_t *r)
{
	srjson_t *jrow, *e;
	int nrows, i, j;
	int built = 0; /* rows that own memory and must be freed on error */

	nrows = (rows == NULL) ? 0 : srjson_GetArraySize(jd, rows);

	RES_ROW_N(r) = nrows;
	if(nrows == 0) {
		RES_ROWS(r) = NULL;
		return 0;
	}

	if(RES_COL_N(r) <= 0) {
		LM_ERR("reply carries %d rows but no column metadata\n", nrows);
		RES_ROW_N(r) = 0;
		return -1;
	}

	/* db_allocate_rows sizes from RES_ROW_N, so it must be set first */
	if(db_allocate_rows(r) != 0) {
		LM_ERR("failed to allocate %d rows\n", nrows);
		RES_ROW_N(r) = 0;
		return -1;
	}

	for(i = 0; i < nrows; i++) {
		jrow = srjson_GetArrayItem(jd, rows, i);
		if(jrow == NULL)
			goto error;

		if(db_allocate_row(r, &(RES_ROWS(r)[i])) != 0) {
			LM_ERR("failed to allocate row %d\n", i);
			goto error;
		}
		/* the row owns a value array from here on */
		built = i + 1;

		for(j = 0; j < RES_COL_N(r); j++) {
			e = srjson_GetArrayItem(jd, jrow, j);
			if(nats_dec_val(
					   e, RES_TYPES(r)[j], &(ROW_VALUES(&(RES_ROWS(r)[i]))[j]))
					< 0) {
				LM_ERR("failed to decode row %d column %d\n", i, j);
				goto error;
			}
		}
	}

	return 0;

error:
	/* truncate to what was built so db_free_rows stays consistent */
	RES_ROW_N(r) = built;
	db_free_rows(r);
	return -1;
}

int db_nats_dec_reply(const db1_con_t *_h, str *reply, db1_res_t **_r)
{
	srjson_doc_t jd;
	srjson_t *root, *it;
	nats_con_t *con = CON_NATS(_h);
	db1_res_t *r = NULL;
	int fresh = 0;
	int ret = -1;

	srjson_InitDoc(&jd, &nats_json_hooks);
	jd.root = srjson_Parse(&jd, reply->s);
	if(jd.root == NULL) {
		LM_ERR("failed to parse reply json\n");
		srjson_DestroyDoc(&jd);
		return -1;
	}
	root = jd.root;

	/* status first */
	it = srjson_GetObjectItem(&jd, root, "ok");
	if(it == NULL || it->type != srjson_True) {
		srjson_t *code = srjson_GetObjectItem(&jd, root, "code");
		srjson_t *err = srjson_GetObjectItem(&jd, root, "err");
		const char *cs = (code && code->valuestring) ? code->valuestring : "?";

		if(strcmp(cs, "no_cursor") == 0) {
			/* the cursor lives in one backend process; say why it is gone */
			LM_ERR("cursor is no longer available on the backend. The owning "
				   "instance may have restarted, the cursor may have passed "
				   "its idle TTL, or the backend may be too old to advertise "
				   "cursor_subject - in which case running more than one "
				   "backend misroutes the fetch.\n");
		} else {
			LM_ERR("backend rejected request: code=[%s] err=[%s]\n", cs,
					(err && err->valuestring) ? err->valuestring : "?");
		}
		goto done;
	}

	it = srjson_GetObjectItem(&jd, root, "affected");
	if(it != NULL)
		con->affected_rows = SRJSON_GET_INT(it);

	/* a cursor id supersedes whatever we held before */
	it = srjson_GetObjectItem(&jd, root, "cursor");
	if(it != NULL && it->valuestring != NULL) {
		int cl = strlen(it->valuestring);
		db_nats_cursor_reset(con);
		con->cursor = (char *)pkg_malloc(cl + 1);
		if(con->cursor == NULL) {
			PKG_MEM_ERROR;
			goto done;
		}
		memcpy(con->cursor, it->valuestring, cl);
		con->cursor[cl] = '\0';

		/* the owning instance's subject travels with the cursor id */
		it = srjson_GetObjectItem(&jd, root, "cursor_subject");
		if(it != NULL && it->valuestring != NULL) {
			int sl = strlen(it->valuestring);
			con->cursor_subject = (char *)pkg_malloc(sl + 1);
			if(con->cursor_subject == NULL) {
				PKG_MEM_ERROR;
				goto done;
			}
			memcpy(con->cursor_subject, it->valuestring, sl);
			con->cursor_subject[sl] = '\0';
		}
	}

	/* the backend signals exhaustion explicitly */
	it = srjson_GetObjectItem(&jd, root, "done");
	if(it != NULL && it->type == srjson_True)
		db_nats_cursor_reset(con);

	if(_r == NULL) {
		ret = 0;
		goto done;
	}

	if(*_r == NULL) {
		r = db_new_result();
		if(r == NULL) {
			LM_ERR("failed to allocate result\n");
			goto done;
		}
		fresh = 1;

		it = srjson_GetObjectItem(&jd, root, "cols");
		if(it != NULL && nats_dec_cols(&jd, it, r) < 0)
			goto done;
	} else {
		/* refilling a fetch page - columns are already in place */
		r = *_r;
		db_free_rows(r);
		RES_ROWS(r) = NULL;
		RES_ROW_N(r) = 0;
	}

	it = srjson_GetObjectItem(&jd, root, "rows");
	if(nats_dec_rows(&jd, it, r) < 0)
		goto done;

	RES_LAST_ROW(r) += RES_ROW_N(r);
	*_r = r;
	ret = 0;

done:
	if(ret < 0 && fresh && r != NULL) {
		db_free_result(r);
		if(_r)
			*_r = NULL;
	}
	srjson_DestroyDoc(&jd);
	return ret;
}
