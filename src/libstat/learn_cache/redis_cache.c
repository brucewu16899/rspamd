/*
 * Copyright (c) 2016, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in the
 *	   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "learn_cache.h"
#include "rspamd.h"
#include "stat_api.h"
#include "stat_internal.h"
#include "cryptobox.h"
#include "ucl.h"
#include "hiredis.h"
#include "adapters/libevent.h"

#define REDIS_DEFAULT_TIMEOUT 0.5
#define REDIS_STAT_TIMEOUT 30
#define REDIS_DEFAULT_PORT 6379
#define DEFAULT_REDIS_KEY "learned_ids"

struct rspamd_redis_cache_ctx {
	struct rspamd_statfile_config *stcf;
	struct upstream_list *read_servers;
	struct upstream_list *write_servers;
	const gchar *redis_object;
	gdouble timeout;
};

struct rspamd_redis_cache_runtime {
	struct rspamd_redis_cache_ctx *ctx;
	struct rspamd_task *task;
	struct upstream *selected;
	struct event timeout_event;
	redisAsyncContext *redis;
};

static GQuark
rspamd_stat_cache_redis_quark (void)
{
	return g_quark_from_static_string ("redis-statistics");
}

/* Called on connection termination */
static void
rspamd_redis_cache_fin (gpointer data)
{
	struct rspamd_redis_cache_runtime *rt = data;

	event_del (&rt->timeout_event);
	redisAsyncFree (rt->redis);
}

static void
rspamd_redis_cache_timeout (gint fd, short what, gpointer d)
{
	struct rspamd_redis_cache_runtime *rt = d;
	struct rspamd_task *task;

	task = rt->task;

	msg_err_task ("connection to redis server %s timed out",
			rspamd_upstream_name (rt->selected));
	rspamd_upstream_fail (rt->selected);
	rspamd_session_remove_event (task->s, rspamd_redis_cache_fin, d);
}

/* Called when we have checked the specified message id */
static void
rspamd_stat_cache_redis_get (redisAsyncContext *c, gpointer r, gpointer priv)
{
	struct rspamd_redis_cache_runtime *rt = priv;
	redisReply *reply = r;
	struct rspamd_task *task;
	glong val = 0;

	task = rt->task;

	if (c->err == 0) {
		if (G_LIKELY (reply->type == REDIS_REPLY_INTEGER)) {
			val = reply->integer;
		}
		else if (reply->type == REDIS_REPLY_STRING) {
			rspamd_strtol (reply->str, reply->len, &val);
		}
		else {
			if (reply->type != REDIS_REPLY_NIL) {
				msg_err_task ("bad learned type for %s: %d",
					rt->ctx->stcf->symbol, reply->type);
			}

			val = 0;
		}

		if ((val > 0 && (task->flags & RSPAMD_TASK_FLAG_LEARN_SPAM)) ||
				(val < 0 && (task->flags & RSPAMD_TASK_FLAG_LEARN_HAM))) {
			/* Already learned */
			g_set_error (&task->err, rspamd_stat_quark (), 404,
					"<%s> has been already "
					"learned as %s, ignore it", task->message_id,
					(task->flags & RSPAMD_TASK_FLAG_LEARN_SPAM) ? "spam" : "ham");
			task->flags |= RSPAMD_TASK_FLAG_ALREADY_LEARNED;
		}
		else if (val != 0) {
			/* Unlearn flag */
			task->flags |= RSPAMD_TASK_FLAG_UNLEARN;
		}
		rspamd_upstream_ok (rt->selected);
	}
	else {
		rspamd_upstream_fail (rt->selected);
	}

	rspamd_session_remove_event (task->s, rspamd_redis_cache_fin, rt);
}

/* Called when we have learned the specified message id */
static void
rspamd_stat_cache_redis_set (redisAsyncContext *c, gpointer r, gpointer priv)
{
	struct rspamd_redis_cache_runtime *rt = priv;
	struct rspamd_task *task;

	task = rt->task;

	if (c->err == 0) {
		/* XXX: we ignore results here */
		rspamd_upstream_ok (rt->selected);
	}
	else {
		rspamd_upstream_fail (rt->selected);
	}

	rspamd_session_remove_event (task->s, rspamd_redis_cache_fin, rt);
}

static void
rspamd_stat_cache_redis_generate_id (struct rspamd_task *task)
{
	rspamd_cryptobox_hash_state_t st;
	rspamd_token_t *tok;
	guint i;
	guchar out[rspamd_cryptobox_HASHBYTES];
	gchar *b32out;
	gchar *user = NULL;

	rspamd_cryptobox_hash_init (&st, NULL, 0);

	user = rspamd_mempool_get_variable (task->task_pool, "stat_user");
	/* Use dedicated hash space for per users cache */
	if (user != NULL) {
		rspamd_cryptobox_hash_update (&st, user, strlen (user));
	}

	for (i = 0; i < task->tokens->len; i ++) {
		tok = g_ptr_array_index (task->tokens, i);
		rspamd_cryptobox_hash_update (&st, tok->data, tok->datalen);
	}

	rspamd_cryptobox_hash_final (&st, out);

	b32out = rspamd_encode_base32 (out, sizeof (out));
	g_assert (b32out != NULL);
	rspamd_mempool_set_variable (task->task_pool, "words_hash", b32out, g_free);
}

gpointer
rspamd_stat_cache_redis_init (struct rspamd_stat_ctx *ctx,
		struct rspamd_config *cfg,
		struct rspamd_statfile *st,
		const ucl_object_t *cf)
{
	struct rspamd_redis_cache_ctx *cache_ctx;
	struct rspamd_statfile_config *stf = st->stcf;
	const ucl_object_t *elt;

	cache_ctx = g_slice_alloc0 (sizeof (*cache_ctx));

	elt = ucl_object_find_key (stf->opts, "read_servers");
	if (elt == NULL) {
		elt = ucl_object_find_key (stf->opts, "servers");
	}
	if (elt == NULL) {
		msg_err ("statfile %s has no redis servers", stf->symbol);

		return NULL;
	}
	else {
		cache_ctx->read_servers = rspamd_upstreams_create (cfg->ups_ctx);
		if (!rspamd_upstreams_from_ucl (cache_ctx->read_servers, elt,
				REDIS_DEFAULT_PORT, NULL)) {
			msg_err ("statfile %s cannot read servers configuration",
					stf->symbol);
			return NULL;
		}
	}

	elt = ucl_object_find_key (stf->opts, "write_servers");
	if (elt == NULL) {
		msg_err ("statfile %s has no write redis servers, "
				"so learning is impossible", stf->symbol);
		cache_ctx->write_servers = NULL;
	}
	else {
		cache_ctx->write_servers = rspamd_upstreams_create (cfg->ups_ctx);
		if (!rspamd_upstreams_from_ucl (cache_ctx->write_servers, elt,
				REDIS_DEFAULT_PORT, NULL)) {
			msg_err ("statfile %s cannot write servers configuration",
					stf->symbol);
			rspamd_upstreams_destroy (cache_ctx->write_servers);
			cache_ctx->write_servers = NULL;
		}
	}

	elt = ucl_object_find_key (stf->opts, "key");
	if (elt == NULL || ucl_object_type (elt) != UCL_STRING) {
		cache_ctx->redis_object = DEFAULT_REDIS_KEY;
	}
	else {
		cache_ctx->redis_object = ucl_object_tostring (elt);
	}

	elt = ucl_object_find_key (stf->opts, "timeout");
	if (elt) {
		cache_ctx->timeout = ucl_object_todouble (elt);
	}
	else {
		cache_ctx->timeout = REDIS_DEFAULT_TIMEOUT;
	}

	cache_ctx->stcf = stf;

	return (gpointer)cache_ctx;
}

gpointer
rspamd_stat_cache_redis_runtime (struct rspamd_task *task,
		gpointer c, gboolean learn)
{
	struct rspamd_redis_cache_ctx *ctx = c;
	struct rspamd_redis_cache_runtime *rt;
	struct upstream *up;
	rspamd_inet_addr_t *addr;

	g_assert (ctx != NULL);

	if (learn && ctx->write_servers == NULL) {
		msg_err_task ("no write servers defined for %s, cannot learn",
				ctx->stcf->symbol);
		return NULL;
	}

	if (learn) {
		up = rspamd_upstream_get (ctx->write_servers,
				RSPAMD_UPSTREAM_MASTER_SLAVE,
				NULL,
				0);
	}
	else {
		up = rspamd_upstream_get (ctx->read_servers,
				RSPAMD_UPSTREAM_ROUND_ROBIN,
				NULL,
				0);
	}

	if (up == NULL) {
		msg_err_task ("no upstreams reachable");
		return NULL;
	}

	rt = rspamd_mempool_alloc0 (task->task_pool, sizeof (*rt));
	rt->selected = up;
	rt->task = task;
	rt->ctx = ctx;

	addr = rspamd_upstream_addr (up);
	g_assert (addr != NULL);
	rt->redis = redisAsyncConnect (rspamd_inet_address_to_string (addr),
			rspamd_inet_address_get_port (addr));
	g_assert (rt->redis != NULL);

	redisLibeventAttach (rt->redis, task->ev_base);

	/* Now check stats */
	event_set (&rt->timeout_event, -1, EV_TIMEOUT, rspamd_redis_cache_timeout, rt);
	event_base_set (task->ev_base, &rt->timeout_event);

	if (!learn) {
		rspamd_stat_cache_redis_generate_id (task);
	}

	return rt;
}

gint
rspamd_stat_cache_redis_check (struct rspamd_task *task,
		gboolean is_spam,
		gpointer runtime)
{
	struct rspamd_redis_cache_runtime *rt = runtime;
	struct timeval tv;
	gchar *h;

	h = rspamd_mempool_get_variable (task->task_pool, "words_hash");
	g_assert (h != NULL);

	double_to_tv (rt->ctx->timeout, &tv);

	if (redisAsyncCommand (rt->redis, rspamd_stat_cache_redis_get, rt,
			"HGET %s %s",
			rt->ctx->redis_object, h) == REDIS_OK) {
		rspamd_session_add_event (task->s, rspamd_redis_cache_fin, rt,
				rspamd_stat_cache_redis_quark ());
		event_add (&rt->timeout_event, &tv);
	}

	/* We need to return OK every time */
	return RSPAMD_LEARN_OK;
}

gint
rspamd_stat_cache_redis_learn (struct rspamd_task *task,
		gboolean is_spam,
		gpointer runtime)
{
	struct rspamd_redis_cache_runtime *rt = runtime;
	struct timeval tv;
	gchar *h;
	gint flag;

	h = rspamd_mempool_get_variable (task->task_pool, "words_hash");
	g_assert (h != NULL);

	double_to_tv (rt->ctx->timeout, &tv);
	flag = (task->flags & RSPAMD_TASK_FLAG_LEARN_SPAM) ? 1 : -1;

	if (redisAsyncCommand (rt->redis, rspamd_stat_cache_redis_set, rt,
			"HSET %s %s %d",
			rt->ctx->redis_object, h, flag) == REDIS_OK) {
		rspamd_session_add_event (task->s, rspamd_redis_cache_fin, rt,
				rspamd_stat_cache_redis_quark ());
		event_add (&rt->timeout_event, &tv);
	}

	/* We need to return OK every time */
	return RSPAMD_LEARN_OK;
}

void
rspamd_stat_cache_redis_close (gpointer c)
{

}