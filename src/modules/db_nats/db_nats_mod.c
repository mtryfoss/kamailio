/*
 * Copyright (C) 2026 Morten Tryfoss
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "../../core/sr_module.h"
#include "../../core/dprint.h"
#include "../../lib/srdb1/db.h"
#include "../../lib/srdb1/db_query.h"

#include "db_nats_mod.h"
#include "nats_connection.h"
#include "nats_dbase.h"

MODULE_VERSION

int db_nats_request_timeout = 1000; /* ms */
int db_nats_connect_timeout = 2000; /* ms */
int db_nats_session_sql_mode = DB_NATS_SESSION_REJECT;
int db_nats_error_penalty = 1000; /* ms */
int db_nats_error_threshold = 1;  /* consecutive errors, per process */
int db_nats_max_reconnect = 10000;
int db_nats_reconnect_wait = 2000; /* ms */

str db_nats_subject_prefix = str_init("kamailio.db");

char *db_nats_servers[DB_NATS_MAX_SERVERS];
int db_nats_servers_n = 0;

static char *db_nats_session_sql_mode_s = NULL;

static int mod_init(void);
static int db_nats_server_add(modparam_t type, void *val);
static int db_nats_bind_api(db_func_t *dbb);

static cmd_export_t cmds[] = {
		{"db_bind_api", (cmd_function)db_nats_bind_api, 0, 0, 0, 0},
		{0, 0, 0, 0, 0, 0}};

static param_export_t params[] = {
		{"request_timeout", PARAM_INT, &db_nats_request_timeout},
		{"connect_timeout", PARAM_INT, &db_nats_connect_timeout},
		{"session_sql_mode", PARAM_STRING, &db_nats_session_sql_mode_s},
		{"error_penalty", PARAM_INT, &db_nats_error_penalty},
		{"error_threshold", PARAM_INT, &db_nats_error_threshold},
		{"max_reconnect", PARAM_INT, &db_nats_max_reconnect},
		{"reconnect_wait", PARAM_INT, &db_nats_reconnect_wait},
		{"subject_prefix", PARAM_STR, &db_nats_subject_prefix},
		{"nats_url", PARAM_STRING | PARAM_USE_FUNC, (void *)db_nats_server_add},
		{0, 0, 0}};

struct module_exports exports = {
		"db_nats",		 /* module name */
		DEFAULT_DLFLAGS, /* dlopen flags */
		cmds,			 /* exported functions */
		params,			 /* exported parameters */
		0,				 /* RPC method exports */
		0,				 /* exported pseudo-variables */
		0,				 /* response handling function */
		mod_init,		 /* module initialization function */
		0,				 /* per-child init function */
		0				 /* module destroy function */
};

/*
 * Modules whose correctness depends on SQL session state that db_nats cannot
 * provide. Rather than refusing merely because the module is loaded, read its
 * configured URL and only act when it actually points at us.
 *
 * This is safe in mod_init (and only there): yyparse() loads every module and
 * assigns every modparam before init_modules() runs any mod_init.
 */
static struct
{
	const char *mod;
	const char *param;
	const char *why;
} db_nats_incompat[] = {
		{"ims_usrloc_scscf", "db_url",
				"it wraps a multi-statement expiry cascade in START "
				"TRANSACTION/COMMIT issued through raw_query"},
		{"p_usrloc", "write_db_url",
				"its failover path uses SERIALIZABLE transactions with a real "
				"rollback to claim a spare database, and is not gated by "
				"db_use_transactions"},
		{"p_usrloc", "read_db_url",
				"its failover path uses SERIALIZABLE transactions with a real "
				"rollback to claim a spare database, and is not gated by "
				"db_use_transactions"},
		{NULL, NULL, NULL}};

/*! \brief Does this db_url resolve to the db_nats driver? */
static int db_nats_url_is_ours(str *u)
{
	if(u == NULL || u->s == NULL || u->len <= 0)
		return 0;
	if(u->len >= 7 && strncasecmp(u->s, "nats://", 7) == 0)
		return 1;
	/* db_bind_mod also accepts a bare module scheme */
	if(u->len == 4 && strncasecmp(u->s, "nats", 4) == 0)
		return 1;
	return 0;
}

static int db_nats_check_incompat(void)
{
	struct sr_module *m;
	modparam_t ptype;
	void *p;
	str u;
	int i;
	int bad = 0;

	for(i = 0; db_nats_incompat[i].mod != NULL; i++) {
		m = find_module_by_name((char *)db_nats_incompat[i].mod);
		if(m == NULL)
			continue;

		p = find_param_export(m, (char *)db_nats_incompat[i].param,
				PARAM_STR | PARAM_STRING, &ptype);
		if(p == NULL)
			continue;

		if(PARAM_TYPE_MASK(ptype) & PARAM_STR) {
			u = *(str *)p;
		} else {
			u.s = *(char **)p;
			u.len = (u.s != NULL) ? strlen(u.s) : 0;
		}

		/* not configured, or pointing at some other driver - not our problem */
		if(!db_nats_url_is_ours(&u))
			continue;

		if(db_nats_session_sql_mode == DB_NATS_SESSION_IGNORE) {
			LM_WARN("module [%s] has %s=[%.*s] but %s. With "
					"session_sql_mode=\"ignore\" those statements become "
					"no-ops, so it loses atomicity and isolation.\n",
					db_nats_incompat[i].mod, db_nats_incompat[i].param, u.len,
					u.s, db_nats_incompat[i].why);
		} else {
			LM_ERR("module [%s] has %s=[%.*s] but %s. db_nats is stateless "
				   "request/reply and cannot give it connection affinity. "
				   "Point that module at a different driver, or set "
				   "session_sql_mode=\"ignore\" to accept the loss of "
				   "atomicity.\n",
					db_nats_incompat[i].mod, db_nats_incompat[i].param, u.len,
					u.s, db_nats_incompat[i].why);
			bad = 1;
		}
	}

	return bad ? -1 : 0;
}

/*!
 * \brief Collect one cluster member from a nats_url parameter.
 *
 * Repeatable, in the same style as the nats module, so a cluster is written
 * as several nats_url lines rather than a list the srdb1 URL parser cannot
 * represent.
 */
static int db_nats_server_add(modparam_t type, void *val)
{
	char *url = (char *)val;
	int len;

	if(url == NULL) {
		LM_ERR("null nats_url\n");
		return -1;
	}
	len = strlen(url);

	if(len == 0 || len > DB_NATS_URL_MAX_SIZE) {
		LM_ERR("nats_url must be 1..%d characters\n", DB_NATS_URL_MAX_SIZE);
		return -1;
	}
	if(strncmp(url, "nats://", 7) != 0) {
		LM_ERR("invalid nats_url [%s], expected a nats:// url\n", url);
		return -1;
	}
	if(db_nats_servers_n >= DB_NATS_MAX_SERVERS) {
		LM_ERR("too many nats_url entries, max is %d\n", DB_NATS_MAX_SERVERS);
		return -1;
	}

	/* runs before fork, so children inherit the list */
	db_nats_servers[db_nats_servers_n] = (char *)pkg_malloc(len + 1);
	if(db_nats_servers[db_nats_servers_n] == NULL) {
		PKG_MEM_ERROR;
		return -1;
	}
	memcpy(db_nats_servers[db_nats_servers_n], url, len + 1);
	db_nats_servers_n++;

	/* the url may embed credentials, so never log it verbatim */
	LM_DBG("added nats server [%s] (%d total)\n", nats_url_redacted(url),
			db_nats_servers_n);
	return 0;
}

int mod_register(char *path, int *dlflags, void *p1, void *p2)
{
	if(db_api_init() < 0)
		return -1;
	return 0;
}

static int mod_init(void)
{
	if(db_nats_session_sql_mode_s != NULL) {
		if(strcasecmp(db_nats_session_sql_mode_s, "reject") == 0) {
			db_nats_session_sql_mode = DB_NATS_SESSION_REJECT;
		} else if(strcasecmp(db_nats_session_sql_mode_s, "ignore") == 0) {
			db_nats_session_sql_mode = DB_NATS_SESSION_IGNORE;
		} else {
			LM_ERR("invalid session_sql_mode [%s], expected \"reject\" or "
				   "\"ignore\"\n",
					db_nats_session_sql_mode_s);
			return -1;
		}
	}

	if(db_nats_request_timeout <= 0) {
		LM_ERR("request_timeout must be greater than zero\n");
		return -1;
	}
	if(db_nats_connect_timeout <= 0) {
		LM_ERR("connect_timeout must be greater than zero\n");
		return -1;
	}
	if(db_nats_error_penalty < 0) {
		LM_ERR("error_penalty cannot be negative\n");
		return -1;
	}
	if(db_nats_error_threshold < 1) {
		LM_ERR("error_threshold must be at least 1\n");
		return -1;
	}
	if(db_nats_subject_prefix.len <= 0) {
		LM_ERR("subject_prefix must not be empty\n");
		return -1;
	}

	/* this blocks a SIP worker for the duration of every query */
	if(db_nats_request_timeout > 5000) {
		LM_WARN("request_timeout is %d ms - a blocked worker cannot process "
				"other traffic for that long\n",
				db_nats_request_timeout);
	}

	if(db_nats_check_incompat() < 0)
		return -1;

	if(db_nats_servers_n > 0) {
		LM_INFO("db_nats using %d server(s) from nats_url; the host in db_url "
				"is ignored and only its path selects the subject prefix\n",
				db_nats_servers_n);
	}

	LM_INFO("db_nats initialized: request_timeout=%dms connect_timeout=%dms "
			"session_sql_mode=%s error_penalty=%dms error_threshold=%d "
			"default_subject_prefix=[%.*s]\n",
			db_nats_request_timeout, db_nats_connect_timeout,
			(db_nats_session_sql_mode == DB_NATS_SESSION_IGNORE) ? "ignore"
																 : "reject",
			db_nats_error_penalty, db_nats_error_threshold,
			db_nats_subject_prefix.len, db_nats_subject_prefix.s);

	return 0;
}

static int db_nats_bind_api(db_func_t *dbb)
{
	if(dbb == NULL)
		return -1;

	memset(dbb, 0, sizeof(db_func_t));

	dbb->use_table = db_nats_use_table;
	dbb->init = db_nats_init;
	dbb->init2 = db_nats_init2;
	dbb->close = db_nats_close;
	dbb->query = db_nats_query;
	dbb->fetch_result = db_nats_fetch_result;
	dbb->raw_query = db_nats_raw_query;
	dbb->free_result = db_nats_free_result;
	dbb->insert = db_nats_insert;
	dbb->delete = db_nats_delete;
	dbb->update = db_nats_update;
	dbb->replace = db_nats_replace;
	dbb->insert_update = db_nats_insert_update;
	dbb->affected_rows = db_nats_affected_rows;

	/* deliberately left NULL: transactions, query_lock, last_inserted_id,
	 * insert_delayed, insert_async and raw_query_async all need either
	 * connection affinity or a consumer that does not exist in tree.
	 * db_check_api() derives the capability bits from these pointers. */

	return 0;
}
