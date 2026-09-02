/*
 * Copyright (C) 2026 Morten Tryfoss
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "../../core/dprint.h"
#include "../../core/mem/mem.h"
#include "../../core/pt.h"
#include "../../core/ut.h"

#include "db_nats_mod.h"
#include "nats_connection.h"

/*!
 * \brief Mask the password in a nats URL for logging.
 *
 * Credentials may be embedded as nats://user:pass@host, and connection errors
 * are logged at startup, so the password must not reach the log file.
 * Returns a pointer to a static buffer; for log statements only.
 */
const char *nats_url_redacted(const char *url)
{
	static char buf[DB_NATS_URL_MAX_SIZE + 8];
	const char *at, *colon;
	int n;

	if(url == NULL)
		return "(null)";

	at = strrchr(url, '@');
	if(at == NULL)
		return url; /* no credentials to hide */

	/* find the ':' separating user and password, after the scheme */
	colon = strchr(url + 7, ':');
	if(colon == NULL || colon > at)
		return url; /* user only, no password */

	n = snprintf(buf, sizeof(buf), "%.*s:***%s", (int)(colon - url), url, at);
	if(n < 0 || n >= (int)sizeof(buf))
		return "(url too long)";
	return buf;
}

/*! \brief Monotonic milliseconds, used for the error penalty window. */
static uint64_t nats_now_ms(void)
{
	struct timespec ts;

	if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/*!
 * \brief Start failing fast after a transport error.
 *
 * Without this, every query during an outage pays the full request_timeout,
 * and each one blocks a SIP worker for that long. With it, one request per
 * window probes the connection and the rest fail immediately.
 */
static void nats_penalise(nats_con_t *con)
{
	con->consecutive_errors++;

	if(db_nats_error_penalty <= 0)
		return;
	if(con->consecutive_errors < db_nats_error_threshold)
		return;

	con->penalty_until = nats_now_ms() + (uint64_t)db_nats_error_penalty;
	LM_DBG("nats penalty window opened for %d ms after %d consecutive "
		   "errors in this process\n",
			db_nats_error_penalty, con->consecutive_errors);
}

/*!
 * \brief Reject a db_url the srdb1 parser could not represent.
 *
 * parse_db_url() knows nothing about server lists, so a comma-separated
 * db_url is silently mangled: the port is lost and the remainder is swallowed
 * into the database field, which would leave us connected to a plausible host
 * but publishing on nonsense subjects. Fail loudly instead, and point at the
 * nats_url parameter, which is how a cluster is expressed.
 */
static int nats_validate_id(struct db_id *id)
{
	const char *p;
	int i;

	/* Check the raw URL, not the parsed fields: depending on where the commas
	 * fall, parse_db_url() may scatter a server list across the password or
	 * database field and still leave host looking clean, which would connect
	 * us to a plausible but wrong server with the port silently dropped. No
	 * legitimate nats db_url contains a comma. */
	for(i = 0; i < id->url.len; i++) {
		if(id->url.s[i] == ',') {
			LM_ERR("db_url [%.*s] contains a comma - the database URL cannot "
				   "carry a server list, and would be parsed into something "
				   "else entirely. List cluster members with repeated "
				   "nats_url parameters instead.\n",
					id->url.len, id->url.s);
			return -1;
		}
	}

	if(id->database == NULL)
		return 0;

	/* the path becomes a NATS subject prefix */
	for(p = id->database; *p != '\0'; p++) {
		if((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
				|| (*p >= '0' && *p <= '9') || *p == '.' || *p == '_'
				|| *p == '-')
			continue;
		LM_ERR("db_url path [%s] is not a usable subject prefix (bad "
			   "character '%c'). Expected something like "
			   "nats://host:4222/kamailio.db\n",
				id->database, *p);
		return -1;
	}

	return 0;
}

/*! \brief Assemble "nats://[user[:pass]@]host[:port]" into a pkg buffer. */
static char *nats_build_server_url(struct db_id *id)
{
	char *u;
	int len;
	int n;

	if(id->host == NULL || id->host[0] == '\0') {
		LM_ERR("no host in db_url\n");
		return NULL;
	}

	/* scheme + optional credentials + host + optional :port + NUL */
	len = 7 + strlen(id->host) + 8;
	if(id->username)
		len += strlen(id->username) + 1;
	if(id->password)
		len += strlen(id->password) + 1;

	u = (char *)pkg_malloc(len);
	if(u == NULL) {
		PKG_MEM_ERROR;
		return NULL;
	}

	if(id->username && id->username[0] != '\0') {
		if(id->password && id->password[0] != '\0') {
			n = snprintf(u, len, "nats://%s:%s@%s", id->username, id->password,
					id->host);
		} else {
			n = snprintf(u, len, "nats://%s@%s", id->username, id->host);
		}
	} else {
		n = snprintf(u, len, "nats://%s", id->host);
	}
	if(n < 0 || n >= len)
		goto error;

	if(id->port > 0) {
		if(snprintf(u + n, len - n, ":%u", (unsigned int)id->port) < 0)
			goto error;
	}

	return u;

error:
	LM_ERR("failed to build nats server url\n");
	pkg_free(u);
	return NULL;
}

/*! \brief Build "<prefix>.<suffix>" as a NUL-terminated str in pkg memory. */
static int nats_build_subject(str *dst, str *prefix, const char *suffix)
{
	int len;

	len = prefix->len + 1 + strlen(suffix);
	dst->s = (char *)pkg_malloc(len + 1);
	if(dst->s == NULL) {
		PKG_MEM_ERROR;
		return -1;
	}
	memcpy(dst->s, prefix->s, prefix->len);
	dst->s[prefix->len] = '.';
	memcpy(dst->s + prefix->len + 1, suffix, strlen(suffix));
	dst->s[len] = '\0';
	dst->len = len;
	return 0;
}

/*! \brief Apply the option set. Mirrors the tuning used by the nats module. */
static int nats_setup_options(nats_con_t *con)
{
	natsStatus s;

	s = natsOptions_Create(&con->opts);
	if(s != NATS_OK) {
		LM_ERR("failed to create nats options: %s\n", natsStatus_GetText(s));
		return -1;
	}

	natsOptions_SetAllowReconnect(con->opts, true);
	natsOptions_SetMaxReconnect(con->opts, db_nats_max_reconnect);
	natsOptions_SetReconnectWait(con->opts, db_nats_reconnect_wait);
	natsOptions_SetTimeout(con->opts, db_nats_connect_timeout);
	/* with a NULL callback this blocks and retries synchronously, which
	 * would stall whichever process is opening the connection */
	natsOptions_SetRetryOnFailedConnect(con->opts, false, NULL, NULL);
	natsOptions_SetPingInterval(con->opts, 2 * 60 * 1000);
	natsOptions_SetMaxPingsOut(con->opts, 2);

	if(db_nats_servers_n > 0) {
		/* an explicit cluster from nats_url wins over the db_url host */
		if(natsOptions_SetServers(
				   con->opts, (const char **)db_nats_servers, db_nats_servers_n)
				!= NATS_OK) {
			LM_ERR("failed to set nats servers\n");
			return -1;
		}
	} else if(natsOptions_SetURL(con->opts, con->srv_url) != NATS_OK) {
		LM_ERR("failed to set nats url\n");
		return -1;
	}

	return 0;
}

/*! \brief (Re)open the libnats handle for the current process. */
static int nats_do_connect(nats_con_t *con)
{
	natsStatus s;

	if(con->nc != NULL) {
		natsConnection_Destroy(con->nc);
		con->nc = NULL;
	}

	s = natsConnection_Connect(&con->nc, con->opts);
	if(s != NATS_OK) {
		/* report what we actually dialled, not the ignored db_url host */
		LM_ERR("failed to connect to nats at [%s]: %s\n",
				nats_url_redacted((db_nats_servers_n > 0) ? db_nats_servers[0]
														  : con->srv_url),
				natsStatus_GetText(s));
		if(db_nats_servers_n > 1)
			LM_ERR("(and %d further nats_url server(s))\n",
					db_nats_servers_n - 1);
		con->nc = NULL;
		return -1;
	}

	con->pid = my_pid();
	LM_DBG("connected to nats [%s] in pid [%d]\n",
			nats_url_redacted((db_nats_servers_n > 0) ? db_nats_servers[0]
													  : con->srv_url),
			con->pid);
	return 0;
}

int db_nats_check_pid(nats_con_t *con)
{
	if(con->nc != NULL && con->pid == my_pid())
		return 0;

	if(con->nc != NULL) {
		/* handle inherited across fork(); libnats threads did not survive */
		LM_DBG("nats handle created in pid [%d], now in [%d] - reconnecting\n",
				con->pid, my_pid());
	}
	/* the stale cursor belonged to the parent's session */
	db_nats_cursor_reset(con);
	return nats_do_connect(con);
}

void *db_nats_new_connection(struct db_id *id)
{
	nats_con_t *con;
	str prefix;

	if(id == NULL) {
		LM_ERR("invalid parameter\n");
		return NULL;
	}

	con = (nats_con_t *)pkg_malloc(sizeof(nats_con_t));
	if(con == NULL) {
		PKG_MEM_ERROR;
		return NULL;
	}
	memset(con, 0, sizeof(nats_con_t));
	con->id = id;
	con->ref = 1;

	if(nats_validate_id(id) < 0)
		goto error;

	con->srv_url = nats_build_server_url(id);
	if(con->srv_url == NULL)
		goto error;

	/* the URL path selects the subject namespace */
	if(id->database != NULL && id->database[0] != '\0') {
		prefix.s = id->database;
		prefix.len = strlen(id->database);
	} else {
		prefix = db_nats_subject_prefix;
	}

	if(nats_build_subject(&con->subj_op, &prefix, "op") < 0)
		goto error;
	if(nats_build_subject(&con->subj_raw, &prefix, "raw") < 0)
		goto error;
	if(nats_build_subject(&con->subj_cursor, &prefix, "cursor") < 0)
		goto error;

	if(nats_setup_options(con) < 0)
		goto error;

	if(nats_do_connect(con) < 0)
		goto error;

	LM_INFO("db_nats connection ready: server [%s] subjects [%.*s|%.*s|%.*s]\n",
			(db_nats_servers_n > 0) ? "from nats_url"
									: nats_url_redacted(con->srv_url),
			con->subj_op.len, con->subj_op.s, con->subj_raw.len,
			con->subj_raw.s, con->subj_cursor.len, con->subj_cursor.s);

	return con;

error:
	db_nats_free_connection((struct pool_con *)con);
	return NULL;
}

void db_nats_free_connection(struct pool_con *pcon)
{
	nats_con_t *con = (nats_con_t *)pcon;

	if(con == NULL)
		return;

	/* only the owning process may touch the libnats handle */
	if(con->nc != NULL && con->pid == my_pid()) {
		db_nats_cursor_close(con);
		natsConnection_Destroy(con->nc);
	}
	con->nc = NULL;

	if(con->opts != NULL)
		natsOptions_Destroy(con->opts);
	if(con->srv_url != NULL)
		pkg_free(con->srv_url);
	if(con->subj_op.s != NULL)
		pkg_free(con->subj_op.s);
	if(con->subj_raw.s != NULL)
		pkg_free(con->subj_raw.s);
	if(con->subj_cursor.s != NULL)
		pkg_free(con->subj_cursor.s);
	if(con->cursor != NULL)
		pkg_free(con->cursor);
	if(con->cursor_subject != NULL)
		pkg_free(con->cursor_subject);
	if(con->id != NULL)
		free_db_id(con->id);

	pkg_free(con);
}

int db_nats_request(nats_con_t *con, str *subject, str *payload, str *reply)
{
	natsStatus s;
	natsMsg *msg = NULL;
	const char *data;
	int len;

	if(con == NULL || subject == NULL || payload == NULL || reply == NULL) {
		LM_ERR("invalid parameter\n");
		return -1;
	}

	reply->s = NULL;
	reply->len = 0;

	if(db_nats_check_pid(con) < 0)
		return -1;

	/* Inside a penalty window we do not even try: the connection is known
	 * bad and attempting would cost another request_timeout. */
	if(con->penalty_until != 0) {
		if(nats_now_ms() < con->penalty_until) {
			LM_DBG("nats in error penalty, failing fast on [%.*s]\n",
					subject->len, subject->s);
			return -1;
		}
		/* window elapsed - let this one through as a probe */
		con->penalty_until = 0;
		con->consecutive_errors = 0;
	}

	/* Nothing can answer while we are disconnected, and libnats would simply
	 * buffer the publish until the request timed out. Do not reconnect here:
	 * natsConnection_Connect() can block for a long time, and libnats already
	 * reconnects in the background. */
	if(natsConnection_Status(con->nc) != NATS_CONN_STATUS_CONNECTED) {
		LM_ERR("nats connection is not established, failing fast on [%.*s]\n",
				subject->len, subject->s);
		nats_penalise(con);
		return -1;
	}

	s = natsConnection_Request(&msg, con->nc, subject->s, payload->s,
			payload->len, (int64_t)db_nats_request_timeout);

	if(s != NATS_OK) {
		if(s == NATS_TIMEOUT) {
			LM_ERR("nats request timed out after %d ms on [%.*s]\n",
					db_nats_request_timeout, subject->len, subject->s);
		} else {
			LM_ERR("nats request failed on [%.*s]: %s\n", subject->len,
					subject->s, natsStatus_GetText(s));
		}
		nats_penalise(con);
		return -1;
	}

	data = natsMsg_GetData(msg);
	len = natsMsg_GetDataLength(msg);
	if(data == NULL || len <= 0) {
		LM_ERR("empty reply on [%.*s]\n", subject->len, subject->s);
		natsMsg_Destroy(msg);
		return -1;
	}

	/* srjson_Parse needs a NUL-terminated buffer */
	reply->s = (char *)pkg_malloc(len + 1);
	if(reply->s == NULL) {
		PKG_MEM_ERROR;
		natsMsg_Destroy(msg);
		return -1;
	}
	memcpy(reply->s, data, len);
	reply->s[len] = '\0';
	reply->len = len;

	natsMsg_Destroy(msg);
	con->penalty_until = 0;
	con->consecutive_errors = 0;
	return 0;
}

void db_nats_cursor_reset(nats_con_t *con)
{
	if(con->cursor != NULL) {
		pkg_free(con->cursor);
		con->cursor = NULL;
	}
	if(con->cursor_subject != NULL) {
		pkg_free(con->cursor_subject);
		con->cursor_subject = NULL;
	}
}

str *db_nats_cursor_subj(nats_con_t *con)
{
	static str s;

	/* fall back to the shared subject for a backend that does not advertise
	 * an instance subject - correct only with a single backend */
	if(con->cursor_subject == NULL)
		return &con->subj_cursor;

	s.s = con->cursor_subject;
	s.len = strlen(con->cursor_subject);
	return &s;
}

void db_nats_cursor_close(nats_con_t *con)
{
	static char buf[256];
	str payload;
	str reply;
	int n;

	if(con == NULL || con->cursor == NULL || con->nc == NULL)
		return;

	n = snprintf(buf, sizeof(buf),
			"{\"v\":%d,\"op\":\"close\",\"cursor\":\"%s\"}", DB_NATS_PROTO_VER,
			con->cursor);
	if(n > 0 && n < (int)sizeof(buf)) {
		payload.s = buf;
		payload.len = n;
		/* best effort - the backend also expires cursors on its own */
		if(db_nats_request(con, db_nats_cursor_subj(con), &payload, &reply)
				== 0) {
			if(reply.s)
				pkg_free(reply.s);
		}
	}

	db_nats_cursor_reset(con);
}
