/*
 * Copyright (C) 2026 Morten Tryfoss
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _DB_NATS_CONNECTION_H_
#define _DB_NATS_CONNECTION_H_

#include <stdint.h>

#include <nats/nats.h>

#include "../../core/str.h"
#include "../../lib/srdb1/db_pool.h"
#include "../../lib/srdb1/db_id.h"

/*!
 * \brief A NATS-backed database connection.
 *
 * The first three members MUST mirror struct pool_con so that the srdb1
 * pool helpers (db_do_init / db_do_close) can treat this as a pool entry.
 * \see src/lib/srdb1/db_pool.h
 */
typedef struct nats_con
{
	struct db_id *id;	   /*!< connection identifier (includes pid) */
	unsigned int ref;	   /*!< reference count */
	struct pool_con *next; /*!< next in pool chain */

	int pid; /*!< pid that created the libnats handle */

	natsConnection *nc;
	natsOptions *opts;

	char *srv_url; /*!< nats://[user:pass@]host:port */

	str subj_op;	 /*!< <prefix>.op */
	str subj_raw;	 /*!< <prefix>.raw */
	str subj_cursor; /*!< <prefix>.cursor */

	int affected_rows; /*!< from the most recent write reply */

	/*! \brief Monotonic ms after which requests may be attempted again.
	 * Set when a transport error occurs so that an outage costs one timeout
	 * per penalty window instead of one per query. */
	uint64_t penalty_until;

	/*! \brief Consecutive transport errors seen by THIS process. State is
	 * per-process (pkg memory, and db_id is keyed by pid), so each worker
	 * keeps its own counter and opens its own penalty window. */
	int consecutive_errors;

	char *cursor; /*!< active server-side cursor id, NULL when none */

	/*! \brief Subject of the backend instance owning cursor.
	 * Cursor state is process-local on the backend, so continuation must go
	 * to its owner rather than the load-balanced queue subject. NULL when the
	 * backend did not advertise one, in which case the shared subject is
	 * used and only a single backend can work. */
	char *cursor_subject;
} nats_con_t;

#define CON_NATS(db_con) ((nats_con_t *)((db_con)->tail))

/*! \brief Build a connection for the given id. Used by db_do_init. */
void *db_nats_new_connection(struct db_id *id);

/*! \brief Tear down a connection. Used by db_do_close. */
void db_nats_free_connection(struct pool_con *con);

/*!
 * \brief Ensure the libnats handle belongs to this process.
 *
 * init() is called both pre-fork (main process) and again per child, and
 * libnats spawns internal threads that do not survive fork(). Reconnect when
 * the handle was created by a different pid.
 * \return 0 on success, negative on error
 */
int db_nats_check_pid(nats_con_t *con);

/*!
 * \brief Synchronous request/reply.
 * \param con connection
 * \param subject subject to publish on
 * \param payload request body
 * \param reply filled with a NUL-terminated pkg buffer the caller must free
 * \return 0 on success, negative on error
 */
int db_nats_request(nats_con_t *con, str *subject, str *payload, str *reply);

/*!
 * \brief Mask the password in a nats URL for logging.
 * Returns a static buffer; use only in log statements.
 */
const char *nats_url_redacted(const char *url);

/*! \brief Subject to address cursor continuation to. */
str *db_nats_cursor_subj(nats_con_t *con);

/*! \brief Best-effort close of any open cursor. */
void db_nats_cursor_close(nats_con_t *con);

/*! \brief Drop the stored cursor id without talking to the backend. */
void db_nats_cursor_reset(nats_con_t *con);

#endif
