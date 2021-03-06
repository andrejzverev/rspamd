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
#include "task.h"
#include "rspamd.h"
#include "filter.h"
#include "protocol.h"
#include "message.h"
#include "lua/lua_common.h"
#include "email_addr.h"
#include "composites.h"
#include "stat_api.h"
#include "unix-std.h"
#include "utlist.h"
#include <math.h>

/*
 * Do not print more than this amount of elts
 */
static const int max_log_elts = 7;

static GQuark
rspamd_task_quark (void)
{
	return g_quark_from_static_string ("task-error");
}

/*
 * Create new task
 */
struct rspamd_task *
rspamd_task_new (struct rspamd_worker *worker, struct rspamd_config *cfg)
{
	struct rspamd_task *new_task;

	new_task = g_slice_alloc0 (sizeof (struct rspamd_task));
	new_task->worker = worker;

	if (cfg) {
		new_task->cfg = cfg;
		REF_RETAIN (cfg);

		if (cfg->check_all_filters) {
			new_task->flags |= RSPAMD_TASK_FLAG_PASS_ALL;
		}

		new_task->re_rt = rspamd_re_cache_runtime_new (cfg->re_cache);
	}

	gettimeofday (&new_task->tv, NULL);
	new_task->time_real = rspamd_get_ticks ();
	new_task->time_virtual = rspamd_get_virtual_ticks ();

	new_task->task_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), "task");

	new_task->results = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	rspamd_mempool_add_destructor (new_task->task_pool,
		(rspamd_mempool_destruct_t) g_hash_table_unref,
		new_task->results);

	new_task->raw_headers = g_hash_table_new (rspamd_strcase_hash,
			rspamd_strcase_equal);
	new_task->request_headers = g_hash_table_new_full (rspamd_ftok_icase_hash,
			rspamd_ftok_icase_equal, rspamd_fstring_mapped_ftok_free,
			rspamd_fstring_mapped_ftok_free);
	rspamd_mempool_add_destructor (new_task->task_pool,
		(rspamd_mempool_destruct_t) g_hash_table_unref,
		new_task->request_headers);
	new_task->reply_headers = g_hash_table_new_full (rspamd_ftok_icase_hash,
			rspamd_ftok_icase_equal, rspamd_fstring_mapped_ftok_free,
			rspamd_fstring_mapped_ftok_free);
	rspamd_mempool_add_destructor (new_task->task_pool,
		(rspamd_mempool_destruct_t) g_hash_table_unref,
		new_task->reply_headers);
	rspamd_mempool_add_destructor (new_task->task_pool,
		(rspamd_mempool_destruct_t) g_hash_table_unref,
		new_task->raw_headers);
	new_task->emails = g_hash_table_new (rspamd_url_hash, rspamd_emails_cmp);
	rspamd_mempool_add_destructor (new_task->task_pool,
		(rspamd_mempool_destruct_t) g_hash_table_unref,
		new_task->emails);
	new_task->urls = g_hash_table_new (rspamd_url_hash, rspamd_urls_cmp);
	rspamd_mempool_add_destructor (new_task->task_pool,
		(rspamd_mempool_destruct_t) g_hash_table_unref,
		new_task->urls);
	new_task->parts = g_ptr_array_sized_new (4);
	rspamd_mempool_add_destructor (new_task->task_pool,
			rspamd_ptr_array_free_hard, new_task->parts);
	new_task->text_parts = g_ptr_array_sized_new (2);
	rspamd_mempool_add_destructor (new_task->task_pool,
			rspamd_ptr_array_free_hard, new_task->text_parts);
	new_task->received = g_ptr_array_sized_new (8);
	rspamd_mempool_add_destructor (new_task->task_pool,
			rspamd_ptr_array_free_hard, new_task->received);

	new_task->sock = -1;
	new_task->flags |= (RSPAMD_TASK_FLAG_MIME|RSPAMD_TASK_FLAG_JSON);
	new_task->pre_result.action = METRIC_ACTION_MAX;

	new_task->message_id = new_task->queue_id = "undef";

	return new_task;
}


static void
rspamd_task_reply (struct rspamd_task *task)
{
	if (task->fin_callback) {
		task->fin_callback (task, task->fin_arg);
	}
	else {
		rspamd_protocol_write_reply (task);
	}
}

/*
 * Called if all filters are processed
 * @return TRUE if session should be terminated
 */
gboolean
rspamd_task_fin (void *arg)
{
	struct rspamd_task *task = (struct rspamd_task *) arg;

	/* Task is already finished or skipped */
	if (RSPAMD_TASK_IS_PROCESSED (task)) {
		rspamd_task_reply (task);
		return TRUE;
	}

	if (!rspamd_task_process (task, RSPAMD_TASK_PROCESS_ALL)) {
		rspamd_task_reply (task);
		return TRUE;
	}

	if (RSPAMD_TASK_IS_PROCESSED (task)) {
		rspamd_task_reply (task);
		return TRUE;
	}

	/* One more iteration */
	return FALSE;
}

/*
 * Called if session was restored inside fin callback
 */
void
rspamd_task_restore (void *arg)
{
	/* XXX: not needed now ? */
}

/*
 * Free all structures of worker_task
 */
void
rspamd_task_free (struct rspamd_task *task)
{
	struct mime_part *p;
	struct mime_text_part *tp;
	struct rspamd_email_address *addr;
	guint i;

	if (task) {
		debug_task ("free pointer %p", task);

		for (i = 0; i < task->parts->len; i ++) {
			p = g_ptr_array_index (task->parts, i);
			g_byte_array_free (p->content, TRUE);

			if (p->raw_headers_str) {
				g_free (p->raw_headers_str);
			}

			if (p->raw_headers) {
				g_hash_table_unref (p->raw_headers);
			}
		}

		for (i = 0; i < task->text_parts->len; i ++) {
			tp = g_ptr_array_index (task->text_parts, i);

			if (tp->normalized_words) {
				g_array_free (tp->normalized_words, TRUE);
			}
			if (tp->normalized_hashes) {
				g_array_free (tp->normalized_hashes, TRUE);
			}
		}

		if (task->rcpt_envelope) {
			for (i = 0; i < task->rcpt_envelope->len; i ++) {
				addr = g_ptr_array_index (task->rcpt_envelope, i);
				rspamd_email_address_unref (addr);
			}
		}

		if (task->from_envelope) {
			rspamd_email_address_unref (task->from_envelope);
		}

		if (task->images) {
			g_list_free (task->images);
		}

		if (task->messages) {
			g_list_free (task->messages);
		}

		if (task->http_conn != NULL) {
			rspamd_http_connection_reset (task->http_conn);
			rspamd_http_connection_unref (task->http_conn);
		}

		if (task->settings != NULL) {
			ucl_object_unref (task->settings);
		}

		if (task->client_addr) {
			rspamd_inet_address_destroy (task->client_addr);
		}

		if (task->from_addr) {
			rspamd_inet_address_destroy (task->from_addr);
		}

		if (task->err) {
			g_error_free (task->err);
		}

		if (event_get_base (&task->timeout_ev) != NULL) {
			event_del (&task->timeout_ev);
		}

		if (task->guard_ev) {
			event_del (task->guard_ev);
		}

		if (task->sock != -1) {
			close (task->sock);
		}

		if (task->cfg) {
			rspamd_re_cache_runtime_destroy (task->re_rt);
			REF_RELEASE (task->cfg);
		}

		rspamd_mempool_delete (task->task_pool);
		g_slice_free1 (sizeof (struct rspamd_task), task);
	}
}

struct rspamd_task_map {
	gpointer begin;
	gulong len;
};

static void
rspamd_task_unmapper (gpointer ud)
{
	struct rspamd_task_map *m = ud;

	munmap (m->begin, m->len);
}

gboolean
rspamd_task_load_message (struct rspamd_task *task,
	struct rspamd_http_message *msg, const gchar *start, gsize len)
{
	guint control_len, r;
	struct ucl_parser *parser;
	ucl_object_t *control_obj;
	gchar filepath[PATH_MAX], *fp;
	gint fd, flen;
	gulong offset = 0, shmem_size = 0;
	rspamd_ftok_t srch, *tok;
	gpointer map;
	struct stat st;
	struct rspamd_task_map *m;

	if (msg) {
		rspamd_protocol_handle_headers (task, msg);
	}

	srch.begin = "shm";
	srch.len = 3;
	tok = g_hash_table_lookup (task->request_headers, &srch);

	if (tok) {
		/* Shared memory part */
		r = rspamd_strlcpy (filepath, tok->begin,
				MIN (sizeof (filepath), tok->len + 1));

		rspamd_decode_url (filepath, filepath, r + 1);
		flen = strlen (filepath);

		if (filepath[0] == '"' && flen > 2) {
			/* We need to unquote filepath */
			fp = &filepath[1];
			fp[flen - 2] = '\0';
		}
		else {
			fp = &filepath[0];
		}

		fd = shm_open (fp, O_RDONLY, 00600);

		if (fd == -1) {
			g_set_error (&task->err, rspamd_task_quark(), RSPAMD_PROTOCOL_ERROR,
					"Cannot open shm segment (%s): %s", fp, strerror (errno));
			return FALSE;
		}

		if (fstat (fd, &st) == -1) {
			g_set_error (&task->err, rspamd_task_quark(), RSPAMD_PROTOCOL_ERROR,
					"Cannot stat shm segment (%s): %s", fp, strerror (errno));
			close (fd);

			return FALSE;
		}

		map = mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

		if (map == MAP_FAILED) {
			close (fd);
			g_set_error (&task->err, rspamd_task_quark(), RSPAMD_PROTOCOL_ERROR,
					"Cannot mmap file (%s): %s", fp, strerror (errno));
			return FALSE;
		}

		close (fd);

		srch.begin = "shm-offset";
		srch.len = 10;
		tok = g_hash_table_lookup (task->request_headers, &srch);

		if (tok) {
			rspamd_strtoul (tok->begin, tok->len, &offset);

			if (offset > (gulong)st.st_size) {
				msg_err_task ("invalid offset %ul (%ul available) for shm "
						"segment %s", offset, st.st_size, fp);
				munmap (map, st.st_size);

				return FALSE;
			}
		}

		srch.begin = "shm-length";
		srch.len = 10;
		tok = g_hash_table_lookup (task->request_headers, &srch);
		shmem_size = st.st_size;

		if (tok) {
			rspamd_strtoul (tok->begin, tok->len, &shmem_size);

			if (shmem_size > (gulong)st.st_size) {
				msg_err_task ("invalid length %ul (%ul available) for shm "
						"segment %s", shmem_size, st.st_size, fp);
				munmap (map, st.st_size);

				return FALSE;
			}
		}

		task->msg.begin = ((guchar *)map) + offset;
		task->msg.len = shmem_size;
		task->flags |= RSPAMD_TASK_FLAG_FILE;
		m = rspamd_mempool_alloc (task->task_pool, sizeof (*m));
		m->begin = map;
		m->len = st.st_size;

		msg_info_task ("loaded message from shared memory %s (%ul size, %ul offset)",
				fp, shmem_size, offset);

		rspamd_mempool_add_destructor (task->task_pool, rspamd_task_unmapper, m);

		return TRUE;
	}

	srch.begin = "file";
	srch.len = 4;
	tok = g_hash_table_lookup (task->request_headers, &srch);

	if (tok == NULL) {
		srch.begin = "path";
		srch.len = 4;
		tok = g_hash_table_lookup (task->request_headers, &srch);
	}

	if (tok) {
		debug_task ("want to scan file %T", tok);

		r = rspamd_strlcpy (filepath, tok->begin,
				MIN (sizeof (filepath), tok->len + 1));

		rspamd_decode_url (filepath, filepath, r + 1);
		flen = strlen (filepath);

		if (filepath[0] == '"' && flen > 2) {
			/* We need to unquote filepath */
			fp = &filepath[1];
			fp[flen - 2] = '\0';
		}
		else {
			fp = &filepath[0];
		}

		if (stat (fp, &st) == -1) {
			g_set_error (&task->err, rspamd_task_quark(), RSPAMD_PROTOCOL_ERROR,
					"Invalid file (%s): %s", fp, strerror (errno));
			return FALSE;
		}

		fd = open (fp, O_RDONLY);

		if (fd == -1) {
			g_set_error (&task->err, rspamd_task_quark(), RSPAMD_PROTOCOL_ERROR,
					"Cannot open file (%s): %s", fp, strerror (errno));
			return FALSE;
		}

		map = mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);


		if (map == MAP_FAILED) {
			close (fd);
			g_set_error (&task->err, rspamd_task_quark(), RSPAMD_PROTOCOL_ERROR,
					"Cannot mmap file (%s): %s", fp, strerror (errno));
			return FALSE;
		}

		close (fd);
		task->msg.begin = map;
		task->msg.len = st.st_size;
		task->flags |= RSPAMD_TASK_FLAG_FILE;

		msg_info_task ("loaded message from file %s", fp);
		m = rspamd_mempool_alloc (task->task_pool, sizeof (*m));
		m->begin = map;
		m->len = st.st_size;

		rspamd_mempool_add_destructor (task->task_pool, rspamd_task_unmapper, m);

		return TRUE;
	}

	/* Plain data */
	debug_task ("got input of length %z", task->msg.len);
	task->msg.begin = start;
	task->msg.len = len;

	if (task->msg.len == 0) {
		task->flags |= RSPAMD_TASK_FLAG_EMPTY;
	}

	if (task->flags & RSPAMD_TASK_FLAG_HAS_CONTROL) {
		/* We have control chunk, so we need to process it separately */
		if (task->msg.len < task->message_len) {
			msg_warn_task ("message has invalid message length: %ul and total len: %ul",
					task->message_len, task->msg.len);
			g_set_error (&task->err, rspamd_task_quark(), RSPAMD_PROTOCOL_ERROR,
					"Invalid length");
			return FALSE;
		}
		control_len = task->msg.len - task->message_len;

		if (control_len > 0) {
			parser = ucl_parser_new (UCL_PARSER_KEY_LOWERCASE);

			if (!ucl_parser_add_chunk (parser, task->msg.begin, control_len)) {
				msg_warn_task ("processing of control chunk failed: %s",
						ucl_parser_get_error (parser));
				ucl_parser_free (parser);
			}
			else {
				control_obj = ucl_parser_get_object (parser);
				ucl_parser_free (parser);
				rspamd_protocol_handle_control (task, control_obj);
				ucl_object_unref (control_obj);
			}

			task->msg.begin += control_len;
			task->msg.len -= control_len;
		}
	}

	return TRUE;
}

static gint
rspamd_task_select_processing_stage (struct rspamd_task *task, guint stages)
{
	gint st, mask;

	mask = task->processed_stages;

	if (mask == 0) {
		st = 0;
	}
	else {
		for (st = 1; mask != 1; st ++) {
			mask = (unsigned int)mask >> 1;
		}
	}

	st = 1 << st;

	if (stages & st) {
		return st;
	}
	else if (st < RSPAMD_TASK_STAGE_DONE) {
		/* We assume that the stage that was not requested is done */
		task->processed_stages |= st;
		return rspamd_task_select_processing_stage (task, stages);
	}

	/* We are done */
	return RSPAMD_TASK_STAGE_DONE;
}

static gboolean
rspamd_process_filters (struct rspamd_task *task)
{
	/* Process metrics symbols */
	return rspamd_symbols_cache_process_symbols (task, task->cfg->cache);
}

gboolean
rspamd_task_process (struct rspamd_task *task, guint stages)
{
	gint st;
	gboolean ret = TRUE;
	GError *stat_error = NULL;

	/* Avoid nested calls */
	if (task->flags & RSPAMD_TASK_FLAG_PROCESSING) {
		return TRUE;
	}

	if (RSPAMD_TASK_IS_PROCESSED (task)) {
		return TRUE;
	}

	if (task->pre_result.action != METRIC_ACTION_MAX) {
		/* Skip all if we have result here */
		task->processed_stages |= RSPAMD_TASK_STAGE_DONE;
		msg_info_task ("skip filters, as pre-filter returned %s action",
				rspamd_action_to_str (task->pre_result.action));
		return TRUE;
	}

	task->flags |= RSPAMD_TASK_FLAG_PROCESSING;

	st = rspamd_task_select_processing_stage (task, stages);

	switch (st) {
	case RSPAMD_TASK_STAGE_READ_MESSAGE:
		if (!rspamd_message_parse (task)) {
			ret = FALSE;
		}
		break;

	case RSPAMD_TASK_STAGE_PRE_FILTERS:
		rspamd_lua_call_pre_filters (task);
		break;

	case RSPAMD_TASK_STAGE_FILTERS:
		if (!rspamd_process_filters (task)) {
			ret = FALSE;
		}
		break;

	case RSPAMD_TASK_STAGE_CLASSIFIERS:
	case RSPAMD_TASK_STAGE_CLASSIFIERS_PRE:
	case RSPAMD_TASK_STAGE_CLASSIFIERS_POST:
		if (!RSPAMD_TASK_IS_EMPTY (task)) {
			if (rspamd_stat_classify (task, task->cfg->lua_state, st, &stat_error) ==
					RSPAMD_STAT_PROCESS_ERROR) {
				msg_err_task ("classify error: %e", stat_error);
				g_error_free (stat_error);
			}
		}
		break;

	case RSPAMD_TASK_STAGE_COMPOSITES:
		rspamd_make_composites (task);
		break;

	case RSPAMD_TASK_STAGE_POST_FILTERS:
		rspamd_lua_call_post_filters (task);
		if ((task->flags & RSPAMD_TASK_FLAG_LEARN_AUTO) &&
				!RSPAMD_TASK_IS_EMPTY (task)) {
			rspamd_stat_check_autolearn (task);
		}
		break;

	case RSPAMD_TASK_STAGE_LEARN:
	case RSPAMD_TASK_STAGE_LEARN_PRE:
	case RSPAMD_TASK_STAGE_LEARN_POST:
		if (task->flags & (RSPAMD_TASK_FLAG_LEARN_SPAM|RSPAMD_TASK_FLAG_LEARN_HAM)) {
			if (task->err == NULL) {
				if (!rspamd_stat_learn (task,
						task->flags & RSPAMD_TASK_FLAG_LEARN_SPAM,
						task->cfg->lua_state, task->classifier,
						st, &stat_error)) {

					if (!(task->flags & RSPAMD_TASK_FLAG_LEARN_AUTO)) {
						task->err = stat_error;
					}

					msg_err_task ("learn error: %e", stat_error);
					task->processed_stages |= RSPAMD_TASK_STAGE_DONE;
				}
			}
		}
		break;

	case RSPAMD_TASK_STAGE_DONE:
		task->processed_stages |= RSPAMD_TASK_STAGE_DONE;
		break;

	default:
		/* TODO: not implemented stage */
		break;
	}

	if (RSPAMD_TASK_IS_SKIPPED (task)) {
		task->processed_stages |= RSPAMD_TASK_STAGE_DONE;
	}

	task->flags &= ~RSPAMD_TASK_FLAG_PROCESSING;

	if (!ret || RSPAMD_TASK_IS_PROCESSED (task)) {
		if (!ret) {
			/* Set processed flags */
			task->processed_stages |= RSPAMD_TASK_STAGE_DONE;
		}

		msg_debug_task ("task is processed");

		return ret;
	}

	if (rspamd_session_events_pending (task->s) != 0) {
		/* We have events pending, so we consider this stage as incomplete */
		msg_debug_task ("need more work on stage %d", st);
	}
	else {
		/* Mark the current stage as done and go to the next stage */
		msg_debug_task ("completed stage %d", st);
		task->processed_stages |= st;

		/* Reset checkpoint */
		task->checkpoint = NULL;

		/* Tail recursion */
		return rspamd_task_process (task, stages);
	}

	return ret;
}

struct rspamd_email_address*
rspamd_task_get_sender (struct rspamd_task *task)
{
	return task->from_envelope;
}

static const gchar *
rspamd_task_cache_principal_recipient (struct rspamd_task *task,
		const gchar *rcpt, gsize len)
{
	gchar *rcpt_lc;

	if (rcpt == NULL) {
		return NULL;
	}

	rcpt_lc = rspamd_mempool_alloc (task->task_pool, len + 1);
	rspamd_strlcpy (rcpt_lc, rcpt, len + 1);
	rspamd_str_lc (rcpt_lc, len);

	rspamd_mempool_set_variable (task->task_pool, "recipient", rcpt_lc, NULL);

	return rcpt_lc;
}

const gchar *
rspamd_task_get_principal_recipient (struct rspamd_task *task)
{
	InternetAddress *iaelt = NULL;
	const gchar *val;
	struct rspamd_email_address *addr;

	val = rspamd_mempool_get_variable (task->task_pool, "recipient");

	if (val) {
		return val;
	}

	if (task->deliver_to) {
		return rspamd_task_cache_principal_recipient (task, task->deliver_to,
				strlen (task->deliver_to));
	}
	if (task->rcpt_envelope != NULL) {
		addr = g_ptr_array_index (task->rcpt_envelope, 0);

		if (addr->addr) {
			return rspamd_task_cache_principal_recipient (task, addr->addr,
					addr->addr_len);
		}
	}

#ifdef GMIME24
	InternetAddressMailbox *imb;

	if (task->rcpt_mime != NULL) {
		iaelt = internet_address_list_get_address (task->rcpt_mime, 0);
	}

	imb = INTERNET_ADDRESS_IS_MAILBOX(iaelt) ?
			INTERNET_ADDRESS_MAILBOX (iaelt) : NULL;

	if (imb) {
		val = internet_address_mailbox_get_addr (imb);

		return rspamd_task_cache_principal_recipient (task, val, strlen (val));
	}
#else
	if (task->rcpt_mime != NULL) {
		iaelt = internet_address_list_get_address (task->rcpt_mime);
	}

	if (iaelt) {
		val = internet_address_get_addr (iaelt);

		return rspamd_task_cache_principal_recipient (task, val, strlen (val));
	}
#endif

	return NULL;
}

gboolean
rspamd_learn_task_spam (struct rspamd_task *task,
	gboolean is_spam,
	const gchar *classifier,
	GError **err)
{
	if (is_spam) {
		task->flags |= RSPAMD_TASK_FLAG_LEARN_SPAM;
	}
	else {
		task->flags |= RSPAMD_TASK_FLAG_LEARN_HAM;
	}

	task->classifier = classifier;

	return TRUE;
}

static gboolean
rspamd_task_log_check_condition (struct rspamd_task *task,
		struct rspamd_log_format *lf)
{
	gboolean ret = FALSE;

	switch (lf->type) {
	case RSPAMD_LOG_MID:
		if (task->message_id && strcmp (task->message_id, "undef") != 0) {
			ret = TRUE;
		}
		break;
	case RSPAMD_LOG_QID:
		if (task->queue_id && strcmp (task->queue_id, "undef") != 0) {
			ret = TRUE;
		}
		break;
	case RSPAMD_LOG_USER:
		if (task->user) {
			ret = TRUE;
		}
		break;
	case RSPAMD_LOG_IP:
		if (task->from_addr && rspamd_ip_is_valid (task->from_addr)) {
			ret = TRUE;
		}
		break;
	case RSPAMD_LOG_SMTP_RCPT:
	case RSPAMD_LOG_SMTP_RCPTS:
		if (task->rcpt_envelope && task->rcpt_envelope->len > 0) {
			ret = TRUE;
		}
		break;
	case RSPAMD_LOG_MIME_RCPT:
	case RSPAMD_LOG_MIME_RCPTS:
		if (task->rcpt_mime &&
				internet_address_list_length (task->rcpt_mime) > 0) {
			ret = TRUE;
		}
		break;
	case RSPAMD_LOG_SMTP_FROM:
		if (task->from_envelope) {
			ret = TRUE;
		}
		break;
	case RSPAMD_LOG_MIME_FROM:
		if (task->from_mime &&
				internet_address_list_length (task->from_mime) > 0) {
			ret = TRUE;
		}
		break;
	default:
		ret = TRUE;
		break;
	}

	return ret;
}

/*
 * Sort by symbol's score -> name
 */
static gint
rspamd_task_compare_log_sym (gconstpointer a, gconstpointer b)
{
	const struct symbol *s1 = *(const struct symbol **)a,
			*s2 = *(const struct symbol **)b;
	gdouble w1, w2;


	w1 = fabs (s1->score);
	w2 = fabs (s2->score);

	if (w1 == w2 && s1->name && s2->name) {
		return strcmp (s1->name, s2->name);
	}

	return (w2 - w1) * 1000.0;
}

static rspamd_ftok_t
rspamd_task_log_metric_res (struct rspamd_task *task,
		struct rspamd_log_format *lf)
{
	static gchar scorebuf[32];
	rspamd_ftok_t res = {.begin = NULL, .len = 0};
	struct metric_result *mres;
	GHashTableIter it;
	gboolean first = TRUE;
	gpointer k, v;
	rspamd_fstring_t *symbuf;
	struct symbol *sym;
	GPtrArray *sorted_symbols;
	guint i, j;

	mres = g_hash_table_lookup (task->results, DEFAULT_METRIC);

	if (mres != NULL) {
		switch (lf->type) {
		case RSPAMD_LOG_ISSPAM:
			if (RSPAMD_TASK_IS_SKIPPED (task)) {
				res.begin = "S";
			}
			else if (mres->action == METRIC_ACTION_REJECT) {
				res.begin = "T";
			}
			else {
				res.begin = "F";
			}

			res.len = 1;
			break;
		case RSPAMD_LOG_ACTION:
			res.begin = rspamd_action_to_str (mres->action);
			res.len = strlen (res.begin);
			break;
		case RSPAMD_LOG_SCORES:
			res.len = rspamd_snprintf (scorebuf, sizeof (scorebuf), "%.2f/%.2f",
					mres->score, mres->actions_limits[METRIC_ACTION_REJECT]);
			res.begin = scorebuf;
			break;
		case RSPAMD_LOG_SYMBOLS:
			symbuf = rspamd_fstring_sized_new (128);
			g_hash_table_iter_init (&it, mres->symbols);
			sorted_symbols = g_ptr_array_sized_new (g_hash_table_size (mres->symbols));

			while (g_hash_table_iter_next (&it, &k, &v)) {
				g_ptr_array_add (sorted_symbols, v);
			}

			g_ptr_array_sort (sorted_symbols, rspamd_task_compare_log_sym);

			for (i = 0; i < sorted_symbols->len; i ++) {
				sym = g_ptr_array_index (sorted_symbols, i);

				if (first) {
					rspamd_printf_fstring (&symbuf, "%s", sym->name);
				}
				else {
					rspamd_printf_fstring (&symbuf, ",%s", sym->name);
				}

				if (lf->flags & RSPAMD_LOG_FLAG_SYMBOLS_SCORES) {
					rspamd_printf_fstring (&symbuf, "(%.2f)", sym->score);
				}

				if (lf->flags & RSPAMD_LOG_FLAG_SYMBOLS_PARAMS) {
					GList *cur;

					rspamd_printf_fstring (&symbuf, "{");

					j = 0;

					for (cur = sym->options; cur != NULL; cur = g_list_next (cur)) {
						rspamd_printf_fstring (&symbuf, "%s;", cur->data);

						if (j >= max_log_elts) {
							rspamd_printf_fstring (&symbuf, "...;");
							break;
						}
						j ++;
					}

					rspamd_printf_fstring (&symbuf, "}");
				}

				first = FALSE;
			}

			g_ptr_array_free (sorted_symbols, TRUE);

			rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t)rspamd_fstring_free,
					symbuf);
			res.begin = symbuf->str;
			res.len = symbuf->len;
			break;
		default:
			break;
		}
	}

	return res;
}

static rspamd_fstring_t *
rspamd_task_log_write_var (struct rspamd_task *task, rspamd_fstring_t *logbuf,
		const rspamd_ftok_t *var, const rspamd_ftok_t *content)
{
	rspamd_fstring_t *res = logbuf;
	const gchar *p, *c, *end;

	if (content == NULL) {
		/* Just output variable */
		res = rspamd_fstring_append (res, var->begin, var->len);
	}
	else {
		/* Replace $ with variable value */
		p = content->begin;
		c = p;
		end = p + content->len;

		while (p < end) {
			if (*p == '$') {
				if (p > c) {
					res = rspamd_fstring_append (res, c, p - c);
				}

				res = rspamd_fstring_append (res, var->begin, var->len);
				p ++;
				c = p;
			}
			else {
				p ++;
			}
		}

		if (p > c) {
			res = rspamd_fstring_append (res, c, p - c);
		}
	}

	return res;
}

static rspamd_fstring_t *
rspamd_task_write_ialist (struct rspamd_task *task,
		InternetAddressList *ialist, gint lim,
		struct rspamd_log_format *lf,
		rspamd_fstring_t *logbuf)
{
	rspamd_fstring_t *res = logbuf, *varbuf;
	rspamd_ftok_t var = {.begin = NULL, .len = 0};
	InternetAddressMailbox *iamb;
	InternetAddress *ia = NULL;
	gint i;

	if (lim <= 0) {
		lim = internet_address_list_length (ialist);
	}


	varbuf = rspamd_fstring_new ();

	for (i = 0; i < lim; i++) {
		ia = internet_address_list_get_address (ialist, i);

		if (ia && INTERNET_ADDRESS_IS_MAILBOX (ia)) {
			iamb = INTERNET_ADDRESS_MAILBOX (ia);
			varbuf = rspamd_fstring_append (varbuf, iamb->addr,
					strlen (iamb->addr));
		}

		if (varbuf->len > 0) {
			if (i != lim - 1) {
				varbuf = rspamd_fstring_append (varbuf, ",", 1);
			}
		}

		if (i >= max_log_elts) {
			varbuf = rspamd_fstring_append (varbuf, "...", 3);
			break;
		}
	}

	if (varbuf->len > 0) {
		var.begin = varbuf->str;
		var.len = varbuf->len;
		res = rspamd_task_log_write_var (task, logbuf,
				&var, (const rspamd_ftok_t *) lf->data);
	}

	rspamd_fstring_free (varbuf);

	return res;
}

static rspamd_fstring_t *
rspamd_task_write_addr_list (struct rspamd_task *task,
		GPtrArray *addrs, gint lim,
		struct rspamd_log_format *lf,
		rspamd_fstring_t *logbuf)
{
	rspamd_fstring_t *res = logbuf, *varbuf;
	rspamd_ftok_t var = {.begin = NULL, .len = 0};
	struct rspamd_email_address *addr;
	gint i;

	if (lim <= 0) {
		lim = addrs->len;
	}

	varbuf = rspamd_fstring_new ();

	for (i = 0; i < lim; i++) {
		addr = g_ptr_array_index (addrs, i);

		if (addr->addr) {
			varbuf = rspamd_fstring_append (varbuf, addr->addr, addr->addr_len);
		}

		if (varbuf->len > 0) {
			if (i != lim - 1) {
				varbuf = rspamd_fstring_append (varbuf, ",", 1);
			}
		}

		if (i >= max_log_elts) {
			varbuf = rspamd_fstring_append (varbuf, "...", 3);
			break;
		}
	}

	if (varbuf->len > 0) {
		var.begin = varbuf->str;
		var.len = varbuf->len;
		res = rspamd_task_log_write_var (task, logbuf,
				&var, (const rspamd_ftok_t *) lf->data);
	}

	rspamd_fstring_free (varbuf);

	return res;
}

static rspamd_fstring_t *
rspamd_task_log_variable (struct rspamd_task *task,
		struct rspamd_log_format *lf, rspamd_fstring_t *logbuf)
{
	rspamd_fstring_t *res = logbuf;
	rspamd_ftok_t var = {.begin = NULL, .len = 0};
	static gchar numbuf[32];

	switch (lf->type) {
	/* String vars */
	case RSPAMD_LOG_MID:
		if (task->message_id) {
			var.begin = task->message_id;
			var.len = strlen (var.begin);
		}
		else {
			var.begin = "undef";
			var.len = 5;
		}
		break;
	case RSPAMD_LOG_QID:
		if (task->queue_id) {
			var.begin = task->queue_id;
			var.len = strlen (var.begin);
		}
		else {
			var.begin = "undef";
			var.len = 5;
		}
		break;
	case RSPAMD_LOG_USER:
		if (task->user) {
			var.begin = task->user;
			var.len = strlen (var.begin);
		}
		else {
			var.begin = "undef";
			var.len = 5;
		}
		break;
	case RSPAMD_LOG_IP:
		if (task->from_addr && rspamd_ip_is_valid (task->from_addr)) {
			var.begin = rspamd_inet_address_to_string (task->from_addr);
			var.len = strlen (var.begin);
		}
		else {
			var.begin = "undef";
			var.len = 5;
		}
		break;
	/* Numeric vars */
	case RSPAMD_LOG_LEN:
		var.len = rspamd_snprintf (numbuf, sizeof (numbuf), "%uz",
				task->msg.len);
		var.begin = numbuf;
		break;
	case RSPAMD_LOG_DNS_REQ:
		var.len = rspamd_snprintf (numbuf, sizeof (numbuf), "%uD",
				task->dns_requests);
		var.begin = numbuf;
		break;
	case RSPAMD_LOG_TIME_REAL:
		var.begin = rspamd_log_check_time (task->time_real, rspamd_get_ticks (),
				task->cfg->clock_res);
		var.len = strlen (var.begin);
		break;
	case RSPAMD_LOG_TIME_VIRTUAL:
		var.begin = rspamd_log_check_time (task->time_virtual,
				rspamd_get_virtual_ticks (),
				task->cfg->clock_res);
		var.len = strlen (var.begin);
		break;
	/* InternetAddress vars */
	case RSPAMD_LOG_SMTP_FROM:
		if (task->from_envelope) {
			var.begin = task->from_envelope->addr;
			var.len = task->from_envelope->addr_len;
		}
		break;
	case RSPAMD_LOG_MIME_FROM:
		if (task->from_mime) {
			return rspamd_task_write_ialist (task, task->from_mime, 1, lf,
					logbuf);
		}
		break;
	case RSPAMD_LOG_SMTP_RCPT:
		if (task->rcpt_envelope) {
			return rspamd_task_write_addr_list (task, task->rcpt_envelope, 1, lf,
					logbuf);
		}
		break;
	case RSPAMD_LOG_MIME_RCPT:
		if (task->rcpt_mime) {
			return rspamd_task_write_ialist (task, task->rcpt_mime, 1, lf,
					logbuf);
		}
		break;
	case RSPAMD_LOG_SMTP_RCPTS:
		if (task->rcpt_envelope) {
			return rspamd_task_write_addr_list (task, task->rcpt_envelope, -1, lf,
					logbuf);
		}
		break;
	case RSPAMD_LOG_MIME_RCPTS:
		if (task->rcpt_mime) {
			return rspamd_task_write_ialist (task, task->rcpt_mime, -1, lf,
					logbuf);
		}
		break;
	default:
		var = rspamd_task_log_metric_res (task, lf);
		break;
	}

	if (var.len > 0) {
		res = rspamd_task_log_write_var (task, logbuf,
				&var, (const rspamd_ftok_t *)lf->data);
	}

	return res;
}

void
rspamd_task_write_log (struct rspamd_task *task)
{
	rspamd_fstring_t *logbuf;
	struct rspamd_log_format *lf;
	struct rspamd_task **ptask;
	const gchar *lua_str;
	gsize lua_str_len;
	lua_State *L;

	g_assert (task != NULL);

	if (task->cfg->log_format == NULL ||
			(task->flags & RSPAMD_TASK_FLAG_NO_LOG)) {
		return;
	}

	logbuf = rspamd_fstring_sized_new (1000);

	DL_FOREACH (task->cfg->log_format, lf) {
		switch (lf->type) {
		case RSPAMD_LOG_STRING:
			logbuf = rspamd_fstring_append (logbuf, lf->data, lf->len);
			break;
		case RSPAMD_LOG_LUA:
			L = task->cfg->lua_state;
			lua_rawgeti (L, LUA_REGISTRYINDEX, GPOINTER_TO_INT (lf->data));
			ptask = lua_newuserdata (L, sizeof (*ptask));
			rspamd_lua_setclass (L, "rspamd{task}", -1);
			*ptask = task;

			if (lua_pcall (L, 1, 1, 0) != 0) {
				msg_err_task ("call to log function failed: %s",
						lua_tostring (L, -1));
				lua_pop (L, 1);
			}
			else {
				lua_str = lua_tolstring (L, -1, &lua_str_len);

				if (lua_str != NULL) {
					logbuf = rspamd_fstring_append (logbuf, lua_str, lua_str_len);
				}
				lua_pop (L, 1);
			}
			break;
		default:
			/* We have a variable in log format */
			if (lf->flags & RSPAMD_LOG_FLAG_CONDITION) {
				if (!rspamd_task_log_check_condition (task, lf)) {
					continue;
				}
			}

			logbuf = rspamd_task_log_variable (task, lf, logbuf);
			break;
		}
	}

	msg_info_task ("%V", logbuf);

	rspamd_fstring_free (logbuf);
}
