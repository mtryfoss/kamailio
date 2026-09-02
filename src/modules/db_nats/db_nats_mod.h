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

#ifndef _DB_NATS_MOD_H_
#define _DB_NATS_MOD_H_

/*! \brief how to treat session-state SQL seen on the raw_query path */
#define DB_NATS_SESSION_REJECT 0
#define DB_NATS_SESSION_IGNORE 1

/*! \brief protocol version carried in every request */
#define DB_NATS_PROTO_VER 1

/*! \brief limits for the nats_url parameter, matching the nats module */
#define DB_NATS_MAX_SERVERS 10
#define DB_NATS_URL_MAX_SIZE 256

extern int db_nats_request_timeout;	 /*!< per-request timeout, milliseconds */
extern int db_nats_connect_timeout;	 /*!< connect timeout, milliseconds */
extern int db_nats_session_sql_mode; /*!< DB_NATS_SESSION_* */
extern int db_nats_error_penalty;	 /*!< ms to fail fast after errors */
extern int db_nats_error_threshold; /*!< consecutive errors before penalising */
extern int db_nats_max_reconnect;
extern int db_nats_reconnect_wait;

extern str db_nats_subject_prefix; /*!< fallback when the URL carries no path */

/*!
 * \brief Cluster members given with the nats_url parameter.
 *
 * When at least one is set they replace the host in db_url, which then only
 * supplies the subject prefix. Populated before fork, read-only afterwards.
 */
extern char *db_nats_servers[DB_NATS_MAX_SERVERS];
extern int db_nats_servers_n;

#endif
