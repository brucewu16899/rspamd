/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef RSPAMD_WORKER_PRIVATE_H
#define RSPAMD_WORKER_PRIVATE_H

#include "config.h"
#include "libcryptobox/cryptobox.h"
#include "libcryptobox/keypair.h"
#include "libserver/task.h"
#include "libserver/cfg_file.h"
#include "libserver/rspamd_control.h"

/*
 * Worker's context
 */
struct rspamd_worker_log_pipe {
	gint fd;
	enum rspamd_log_pipe_type type;
	struct rspamd_worker_log_pipe *prev, *next;
};

static const guint64 rspamd_worker_magic = 0xb48abc69d601dc1dULL;

struct rspamd_worker_ctx {
	guint64 magic;
	guint32 timeout;
	struct timeval io_tv;
	/* Detect whether this worker is mime worker    */
	gboolean is_mime;
	/* HTTP worker									*/
	gboolean is_http;
	/* JSON output                                  */
	gboolean is_json;
	/* Allow learning throught worker				*/
	gboolean allow_learn;
	/* DNS resolver */
	struct rspamd_dns_resolver *resolver;
	/* Limit of tasks */
	guint32 max_tasks;
	/* Maximum time for task processing */
	gdouble task_timeout;
	/* Events base */
	struct event_base *ev_base;
	/* Encryption key */
	struct rspamd_cryptobox_keypair *key;
	/* Keys cache */
	struct rspamd_keypair_cache *keys_cache;
	/* Configuration */
	struct rspamd_config *cfg;
	/* Log pipe */
	struct rspamd_worker_log_pipe *log_pipes;
};

#endif
