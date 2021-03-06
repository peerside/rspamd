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

#include "config.h"
#include "task.h"
#include "mime_parser.h"
#include "mime_headers.h"
#include "message.h"
#include "content_type.h"
#include "multipattern.h"
#include "cryptobox.h"
#include "contrib/libottery/ottery.h"

struct rspamd_mime_parser_lib_ctx {
	struct rspamd_multipattern *mp_boundary;
	guchar hkey[rspamd_cryptobox_SIPKEYBYTES]; /* Key for hashing */
	guint key_usages;
} *lib_ctx = NULL;

static const guint max_nested = 32;
static const guint max_key_usages = 10000;

#define msg_debug_mime(...)  rspamd_default_log_function (G_LOG_LEVEL_DEBUG, \
        "mime", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)

#define RSPAMD_MIME_BOUNDARY_FLAG_CLOSED (1 << 0)
#define RSPAMD_BOUNDARY_IS_CLOSED(b) ((b)->flags & RSPAMD_MIME_BOUNDARY_FLAG_CLOSED)

struct rspamd_mime_boundary {
	goffset boundary;
	goffset start;
	guint64 hash;
	guint64 closed_hash;
	gint flags;
};

struct rspamd_mime_parser_ctx {
	GPtrArray *stack; /* Stack of parts */
	GArray *boundaries; /* Boundaries found in the whole message */
	const gchar *start;
	const gchar *pos;
	const gchar *end;
};

static gboolean
rspamd_mime_parse_multipart_part (struct rspamd_task *task,
		struct rspamd_mime_part *part,
		struct rspamd_mime_parser_ctx *st,
		GError **err);
static gboolean
rspamd_mime_parse_message (struct rspamd_task *task,
		struct rspamd_mime_part *part,
		struct rspamd_mime_parser_ctx *st,
		GError **err);
static gboolean
rspamd_mime_parse_normal_part (struct rspamd_task *task,
		struct rspamd_mime_part *part,
		struct rspamd_mime_parser_ctx *st,
		GError **err);


#define RSPAMD_MIME_QUARK (rspamd_mime_parser_quark())
static GQuark
rspamd_mime_parser_quark (void)
{
	return g_quark_from_static_string ("mime-parser");
}

const gchar*
rspamd_cte_to_string (enum rspamd_cte ct)
{
	const gchar *ret = "unknown";

	switch (ct) {
	case RSPAMD_CTE_7BIT:
		ret = "7bit";
		break;
	case RSPAMD_CTE_8BIT:
		ret = "8bit";
		break;
	case RSPAMD_CTE_QP:
		ret = "quoted-printable";
		break;
	case RSPAMD_CTE_B64:
		ret = "base64";
		break;
	default:
		break;
	}

	return ret;
}

enum rspamd_cte
rspamd_cte_from_string (const gchar *str)
{
	enum rspamd_cte ret = RSPAMD_CTE_UNKNOWN;

	g_assert (str != NULL);

	if (strcmp (str, "7bit") == 0) {
		ret = RSPAMD_CTE_7BIT;
	}
	else if (strcmp (str, "8bit") == 0) {
		ret = RSPAMD_CTE_8BIT;
	}
	else if (strcmp (str, "quoted-printable") == 0) {
		ret = RSPAMD_CTE_QP;
	}
	else if (strcmp (str, "base64") == 0) {
		ret = RSPAMD_CTE_B64;
	}

	return ret;
}

static void
rspamd_mime_parser_init_lib (void)
{
	lib_ctx = g_malloc0 (sizeof (*lib_ctx));
	lib_ctx->mp_boundary = rspamd_multipattern_create (RSPAMD_MULTIPATTERN_DEFAULT);
	g_assert (lib_ctx->mp_boundary != NULL);
	rspamd_multipattern_add_pattern (lib_ctx->mp_boundary, "\r--", 0);
	rspamd_multipattern_add_pattern (lib_ctx->mp_boundary, "\n--", 0);
	g_assert (rspamd_multipattern_compile (lib_ctx->mp_boundary, NULL));
	ottery_rand_bytes (lib_ctx->hkey, sizeof (lib_ctx->hkey));
}

static enum rspamd_cte
rspamd_mime_parse_cte (const gchar *in, gsize len)
{
	guint64 h = rspamd_cryptobox_fast_hash_specific (RSPAMD_CRYPTOBOX_XXHASH64,
			in, len, 0xdeadbabe);
	enum rspamd_cte ret = RSPAMD_CTE_UNKNOWN;

	switch (h) {
	case 0xCEDAA7056B4753F7ULL: /* 7bit */
		ret = RSPAMD_CTE_7BIT;
		break;
	case 0x42E0745448B39FC1ULL: /* 8bit */
	case 0x6B169E6B155BADC0ULL: /* binary */
		ret = RSPAMD_CTE_8BIT;
		break;
	case 0x6D69A5BB02A633B0ULL: /* quoted-printable */
		ret = RSPAMD_CTE_QP;
		break;
	case 0x96305588A76DC9A9ULL: /* base64 */
	case 0x171029DE1B0423A9ULL: /* base-64 */
		ret = RSPAMD_CTE_B64;
		break;
	}

	return ret;
}

static enum rspamd_cte
rspamd_mime_part_get_cte_heuristic (struct rspamd_task *task,
		struct rspamd_mime_part *part)
{
	const guint check_len = 128;
	guint real_len, nspaces = 0, neqsign = 0, n8bit = 0, nqpencoded = 0;
	gboolean b64_chars = TRUE;
	const guchar *p, *end;
	enum rspamd_cte ret = RSPAMD_CTE_UNKNOWN;

	real_len = MIN (check_len, part->raw_data.len);
	p = (const guchar *)part->raw_data.begin;
	end = p + part->raw_data.len;

	while (p < end && g_ascii_isspace (*p)) {
		p ++;
	}

	if (end > p + 2) {
		if (*(end - 1) == '=') {
			neqsign ++;
			end --;
		}

		if (*(end - 1) == '=') {
			neqsign ++;
			end --;
		}
	}

	if (end - p > real_len) {
		end = p + real_len;
	}

	while (p < end) {
		if (*p == '\r' || *p == '\n') {
			if (!b64_chars || n8bit || nspaces) {
				break;
			}
		}
		else if (*p == ' ') {
			nspaces ++;
		}
		else if (*p == '=') {
			neqsign ++;
			p ++;

			if (p + 2 < end && g_ascii_isxdigit (*p) && g_ascii_isxdigit (*(p + 1))) {
				p ++;
				nqpencoded ++;
			}

			continue;
		}
		else if (*p >= 0x80) {
			n8bit ++;
			b64_chars = FALSE;
		}
		else if (!(g_ascii_isalnum (*p) || *p == '/' || *p == '+')) {
			b64_chars = FALSE;
		}

		p ++;
	}

	if (b64_chars && neqsign < 2 && nspaces == 0) {
		ret = RSPAMD_CTE_B64;
	}
	else if (n8bit == 0) {
		if (neqsign > 2 && nqpencoded > 2) {
			ret = RSPAMD_CTE_QP;
		}
		else {
			ret = RSPAMD_CTE_7BIT;
		}
	}
	else {
		ret = RSPAMD_CTE_8BIT;
	}

	msg_debug_mime ("detected cte: %s", rspamd_cte_to_string (ret));
	return ret;
}

static void
rspamd_mime_part_get_cte (struct rspamd_task *task, struct rspamd_mime_part *part)
{
	struct rspamd_mime_header *hdr;
	guint i;
	GPtrArray *hdrs;
	enum rspamd_cte cte = RSPAMD_CTE_UNKNOWN;

	hdrs = rspamd_message_get_header_from_hash (part->raw_headers,
			task->task_pool,
			"Content-Transfer-Encoding", FALSE);

	if (hdrs == NULL) {
		part->cte = rspamd_mime_part_get_cte_heuristic (task, part);
		msg_info_task ("detected missing CTE for part as: %s",
				rspamd_cte_to_string (part->cte));
		part->flags |= RSPAMD_MIME_PART_MISSING_CTE;
	}
	else {
		for (i = 0; i < hdrs->len; i ++) {
			gsize hlen;

			hdr = g_ptr_array_index (hdrs, i);
			hlen = strlen (hdr->value);
			rspamd_str_lc (hdr->value, hlen);
			cte = rspamd_mime_parse_cte (hdr->value, hlen);

			if (cte != RSPAMD_CTE_UNKNOWN) {
				part->cte = cte;
				break;
			}
		}

		if (part->cte == RSPAMD_CTE_UNKNOWN) {
			part->cte = rspamd_mime_part_get_cte_heuristic (task, part);

			msg_info_task ("corrected bad CTE for part to: %s",
					rspamd_cte_to_string (part->cte));
		}
		else if (part->cte == RSPAMD_CTE_B64 || part->cte == RSPAMD_CTE_QP) {
			/* Additionally check sanity */
			cte = rspamd_mime_part_get_cte_heuristic (task, part);

			if (cte == RSPAMD_CTE_8BIT) {
				msg_info_task ("incorrect cte specified for part: %s, %s detected",
						rspamd_cte_to_string (part->cte),
						rspamd_cte_to_string (cte));
				part->cte = cte;
				part->flags |= RSPAMD_MIME_PART_BAD_CTE;
			}
		}
		else {
			msg_debug_mime ("processed cte: %s", rspamd_cte_to_string (cte));
		}
	}
}
static void
rspamd_mime_part_get_cd (struct rspamd_task *task, struct rspamd_mime_part *part)
{
	struct rspamd_mime_header *hdr;
	guint i;
	GPtrArray *hdrs;
	struct rspamd_content_disposition *cd = NULL;

	hdrs = rspamd_message_get_header_from_hash (part->raw_headers,
			task->task_pool,
			"Content-Disposition", FALSE);


	if (hdrs == NULL) {
		cd = rspamd_mempool_alloc0 (task->task_pool, sizeof (*cd));
		cd->type = RSPAMD_CT_INLINE;
	}
	else {
		for (i = 0; i < hdrs->len; i ++) {
			gsize hlen;

			hdr = g_ptr_array_index (hdrs, i);
			hlen = strlen (hdr->value);
			cd = rspamd_content_disposition_parse (hdr->value, hlen,
					task->task_pool);

			if (cd) {
				msg_debug_mime ("processed content disposition: %s",
						cd->lc_data);
				break;
			}
		}
	}

	part->cd = cd;
}

static void
rspamd_mime_parser_calc_digest (struct rspamd_mime_part *part)
{
	/* Blake2b applied to string 'rspamd' */
	static const guchar hash_key[] = {
			0xef,0x43,0xae,0x80,0xcc,0x8d,0xc3,0x4c,
			0x6f,0x1b,0xd6,0x18,0x1b,0xae,0x87,0x74,
			0x0c,0xca,0xf7,0x8e,0x5f,0x2e,0x54,0x32,
			0xf6,0x79,0xb9,0x27,0x26,0x96,0x20,0x92,
			0x70,0x07,0x85,0xeb,0x83,0xf7,0x89,0xe0,
			0xd7,0x32,0x2a,0xd2,0x1a,0x64,0x41,0xef,
			0x49,0xff,0xc3,0x8c,0x54,0xf9,0x67,0x74,
			0x30,0x1e,0x70,0x2e,0xb7,0x12,0x09,0xfe,
	};

	if (part->parsed_data.len > 0) {
		rspamd_cryptobox_hash (part->digest,
				part->parsed_data.begin, part->parsed_data.len,
				hash_key, sizeof (hash_key));
	}
}

static gboolean
rspamd_mime_parse_normal_part (struct rspamd_task *task,
		struct rspamd_mime_part *part,
		struct rspamd_mime_parser_ctx *st,
		GError **err)
{
	rspamd_fstring_t *parsed;
	gssize r;

	g_assert (part != NULL);

	rspamd_mime_part_get_cte (task, part);
	rspamd_mime_part_get_cd (task, part);

	switch (part->cte) {
	case RSPAMD_CTE_7BIT:
	case RSPAMD_CTE_8BIT:
	case RSPAMD_CTE_UNKNOWN:
		if (part->ct->flags & RSPAMD_CONTENT_TYPE_MISSING) {
			if (part->cte != RSPAMD_CTE_7BIT) {
				/* We have something that has a missing content-type,
				 * but it has non-7bit characters.
				 *
				 * In theory, it is very unsafe to process it as a text part
				 * as we unlikely get some sane result
				 */
				part->ct->flags &= ~RSPAMD_CONTENT_TYPE_TEXT;
				part->ct->flags |= RSPAMD_CONTENT_TYPE_BROKEN;
			}
		}

		if (IS_CT_TEXT (part->ct)) {
			/* Need to copy text as we have couple of in-place change functions */
			parsed = rspamd_fstring_sized_new (part->raw_data.len);
			parsed->len = part->raw_data.len;
			memcpy (parsed->str, part->raw_data.begin, parsed->len);
			part->parsed_data.begin = parsed->str;
			part->parsed_data.len = parsed->len;
			rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t)rspamd_fstring_free, parsed);
		}
		else {
			part->parsed_data.begin = part->raw_data.begin;
			part->parsed_data.len = part->raw_data.len;
		}
		break;
	case RSPAMD_CTE_QP:
		parsed = rspamd_fstring_sized_new (part->raw_data.len);
		r = rspamd_decode_qp_buf (part->raw_data.begin, part->raw_data.len,
				parsed->str, parsed->allocated);
		g_assert (r != -1);
		parsed->len = r;
		part->parsed_data.begin = parsed->str;
		part->parsed_data.len = parsed->len;
		rspamd_mempool_add_destructor (task->task_pool,
				(rspamd_mempool_destruct_t)rspamd_fstring_free, parsed);
		break;
	case RSPAMD_CTE_B64:
		parsed = rspamd_fstring_sized_new (part->raw_data.len / 4 * 3 + 12);
		rspamd_cryptobox_base64_decode (part->raw_data.begin, part->raw_data.len,
				parsed->str, &parsed->len);
		part->parsed_data.begin = parsed->str;
		part->parsed_data.len = parsed->len;
		rspamd_mempool_add_destructor (task->task_pool,
				(rspamd_mempool_destruct_t)rspamd_fstring_free, parsed);
		break;
	default:
		g_assert_not_reached ();
	}

	g_ptr_array_add (task->parts, part);
	msg_debug_mime ("parsed data part %T/%T of length %z (%z orig), %s cte",
			&part->ct->type, &part->ct->subtype, part->parsed_data.len,
			part->raw_data.len, rspamd_cte_to_string (part->cte));
	rspamd_mime_parser_calc_digest (part);

	return TRUE;
}

struct rspamd_mime_multipart_cbdata {
	struct rspamd_task *task;
	struct rspamd_mime_part *multipart;
	struct rspamd_mime_parser_ctx *st;
	const gchar *part_start;
	rspamd_ftok_t *cur_boundary;
	guint64 bhash;
	GError **err;
};

static gboolean
rspamd_mime_process_multipart_node (struct rspamd_task *task,
		struct rspamd_mime_parser_ctx *st,
		struct rspamd_mime_part *multipart,
		const gchar *start, const gchar *end,
		GError **err)
{
	struct rspamd_content_type *ct, *sel = NULL;
	struct rspamd_mime_header *hdr;
	GPtrArray *hdrs = NULL;
	struct rspamd_mime_part *npart;
	GString str;
	goffset hdr_pos, body_pos;
	guint i;
	gboolean ret = FALSE;


	str.str = (gchar *)start;
	str.len = end - start;

	hdr_pos = rspamd_string_find_eoh (&str, &body_pos);

	if (multipart->specific.mp.children == NULL) {
		multipart->specific.mp.children = g_ptr_array_sized_new (2);
	}

	npart = rspamd_mempool_alloc0 (task->task_pool,
			sizeof (struct rspamd_mime_part));
	npart->parent_part = multipart;
	npart->raw_headers =  g_hash_table_new_full (rspamd_strcase_hash,
			rspamd_strcase_equal, NULL, rspamd_ptr_array_free_hard);
	g_ptr_array_add (multipart->specific.mp.children, npart);

	if (hdr_pos > 0 && hdr_pos < str.len) {
			npart->raw_headers_str = str.str;
			npart->raw_headers_len = hdr_pos;
			npart->raw_data.begin = start + body_pos;
			npart->raw_data.len = (end - start) - body_pos;

			if (task->raw_headers_content.len > 0) {
				rspamd_mime_headers_process (task, npart->raw_headers,
						npart->raw_headers_str,
						npart->raw_headers_len,
						FALSE);
			}

			hdrs = rspamd_message_get_header_from_hash (npart->raw_headers,
					task->task_pool,
					"Content-Type", FALSE);

	}
	else {
		npart->raw_headers_str = 0;
		npart->raw_headers_len = 0;
		npart->raw_data.begin = start;
		npart->raw_data.len = end - start;
	}


	if (hdrs != NULL) {

		for (i = 0; i < hdrs->len; i ++) {
			hdr = g_ptr_array_index (hdrs, i);
			ct = rspamd_content_type_parse (hdr->value, strlen (hdr->value),
					task->task_pool);

			/* Here we prefer multipart content-type or any content-type */
			if (ct) {
				if (sel == NULL) {
					sel = ct;
				}
				else if (ct->flags & RSPAMD_CONTENT_TYPE_MULTIPART) {
					sel = ct;
				}
			}
		}
	}

	if (sel == NULL) {
		sel = rspamd_mempool_alloc0 (task->task_pool, sizeof (*sel));
		RSPAMD_FTOK_ASSIGN (&sel->type, "application");
		RSPAMD_FTOK_ASSIGN (&sel->subtype, "octet-stream");
	}

	npart->ct = sel;

	if (sel->flags & RSPAMD_CONTENT_TYPE_MULTIPART) {
		g_ptr_array_add (st->stack, npart);
		ret = rspamd_mime_parse_multipart_part (task, npart, st, err);
	}
	else if (sel->flags & RSPAMD_CONTENT_TYPE_MESSAGE) {
		g_ptr_array_add (st->stack, npart);

		if ((ret = rspamd_mime_parse_normal_part (task, npart, st, err))) {
			ret = rspamd_mime_parse_message (task, npart, st, err);
		}
	}
	else {
		ret = rspamd_mime_parse_normal_part (task, npart, st, err);
	}

	return ret;
}

static gboolean
rspamd_mime_parse_multipart_cb (struct rspamd_task *task,
		struct rspamd_mime_part *multipart,
		struct rspamd_mime_parser_ctx *st,
		struct rspamd_mime_multipart_cbdata *cb,
		struct rspamd_mime_boundary *b)
{
	const gchar *pos = st->start + b->boundary;

	task = cb->task;

	/* Now check boundary */
	if (!cb->part_start) {
		cb->part_start = st->start + b->start;
		st->pos = cb->part_start;
	}
	else {
		/* We have seen the start of the boundary */
		if (cb->part_start < pos) {
			/* We should have seen some boundary */
			g_assert (cb->cur_boundary != NULL);


			if (!rspamd_mime_process_multipart_node (task, cb->st,
					cb->multipart, cb->part_start, pos, cb->err)) {
				return FALSE;
			}

			/* Go towards the next part */
			cb->part_start = st->start + b->start;
			cb->st->pos = cb->part_start;
		}
		else {
			/* We have an empty boundary, do nothing */
		}
	}

	return TRUE;
}

static gint
rspamd_multipart_boundaries_filter (struct rspamd_task *task,
		struct rspamd_mime_part *multipart,
		struct rspamd_mime_parser_ctx *st,
		struct rspamd_mime_multipart_cbdata *cb)
{
	struct rspamd_mime_boundary *cur;
	goffset last_offset;
	guint i, sel = 0;

	last_offset = (multipart->raw_data.begin - st->start) +
			multipart->raw_data.len;

	/* Find the first offset suitable for this part */
	for (i = 0; i < st->boundaries->len; i ++) {
		cur = &g_array_index (st->boundaries, struct rspamd_mime_boundary, i);

		if (cur->start >= multipart->raw_data.begin - st->start) {
			if (cb->cur_boundary) {
				/* Check boundary */
				msg_debug_mime ("compare %L and %L", cb->bhash, cur->hash);

				if (cb->bhash == cur->hash) {
					sel = i;
					break;
				}
				else if (cb->bhash == cur->closed_hash) {
					/* Not a closing element in fact */
					cur->flags &= ~(RSPAMD_MIME_BOUNDARY_FLAG_CLOSED);
					cur->hash = cur->closed_hash;
					sel = i;
					break;
				}
			}
			else {
				/* Set current boundary */
				cb->cur_boundary = rspamd_mempool_alloc (task->task_pool,
						sizeof (rspamd_ftok_t));
				cb->cur_boundary->begin = st->start + cur->boundary;
				cb->cur_boundary->len = 0;
				cb->bhash = cur->hash;
				sel = i;
				break;
			}
		}
	}

	/* Now we can go forward with boundaries that are same to what we have */
	for (i = sel; i < st->boundaries->len; i ++) {
		cur = &g_array_index (st->boundaries, struct rspamd_mime_boundary, i);

		if (cur->boundary > last_offset) {
			break;
		}

		if (cur->hash == cb->bhash || cur->closed_hash == cb->bhash) {
			if (!rspamd_mime_parse_multipart_cb (task, multipart, st,
					cb, cur)) {
				return FALSE;
			}

			if (cur->closed_hash == cb->bhash) {
				/* We have again fake closed hash */
				cur->flags &= ~(RSPAMD_MIME_BOUNDARY_FLAG_CLOSED);
				cur->hash = cur->closed_hash;
			}

			if (RSPAMD_BOUNDARY_IS_CLOSED (cur)) {
				/* We also might check the next boundary... */
				if (i < st->boundaries->len - 1) {
					cur = &g_array_index (st->boundaries,
							struct rspamd_mime_boundary, i + 1);

					if (cur->hash == cb->bhash) {
						continue;
					}
					else if (cur->closed_hash == cb->bhash) {
						/* We have again fake closed hash */
						cur->flags &= ~(RSPAMD_MIME_BOUNDARY_FLAG_CLOSED);
						cur->hash = cur->closed_hash;
						continue;
					}
				}

				break;
			}
		}
	}

	if (i == st->boundaries->len && cb->cur_boundary) {
		/* Process the last part */
		struct rspamd_mime_boundary fb;

		fb.boundary = last_offset;

		if (!rspamd_mime_parse_multipart_cb (task, multipart, st,
				cb, &fb)) {
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
rspamd_mime_parse_multipart_part (struct rspamd_task *task,
		struct rspamd_mime_part *part,
		struct rspamd_mime_parser_ctx *st,
		GError **err)
{
	struct rspamd_mime_multipart_cbdata cbdata;
	gboolean ret;

	if (st->stack->len > max_nested) {
		g_set_error (err, RSPAMD_MIME_QUARK, E2BIG, "Nesting level is too high: %d",
				st->stack->len);
		return FALSE;
	}

	g_ptr_array_add (task->parts, part);

	st->pos = part->raw_data.begin;
	cbdata.multipart = part;
	cbdata.task = task;
	cbdata.st = st;
	cbdata.part_start = NULL;
	cbdata.err = err;

	if (part->ct->boundary.len > 0) {
		/* We know our boundary */
		cbdata.cur_boundary = &part->ct->boundary;
		rspamd_cryptobox_siphash ((guchar *)&cbdata.bhash,
				cbdata.cur_boundary->begin, cbdata.cur_boundary->len,
				lib_ctx->hkey);
		msg_debug_mime ("hash: %T -> %L", cbdata.cur_boundary, cbdata.bhash);
	}
	else {
		/* Guess boundary */
		cbdata.cur_boundary = NULL;
		cbdata.bhash = 0;
	}

	ret = rspamd_multipart_boundaries_filter (task, part, st, &cbdata);
	/* Cleanup stack */
	g_ptr_array_remove_index_fast (st->stack, st->stack->len - 1);

	return ret;
}

/* Process boundary like structures in a message */
static gint
rspamd_mime_preprocess_cb (struct rspamd_multipattern *mp,
		guint strnum,
		gint match_start,
		gint match_pos,
		const gchar *text,
		gsize len,
		void *context)
{
	const gchar *end = text + len, *p = text + match_pos, *bend;
	gchar *lc_copy;
	gsize blen;
	gboolean closing = FALSE;
	struct rspamd_mime_boundary b;
	struct rspamd_mime_parser_ctx *st = context;

	if (G_LIKELY (p < end)) {
		blen = rspamd_memcspn (p, "\r\n", end - p);

		if (blen > 0) {
			/* We have found something like boundary */
			bend = p + blen - 1;

			if (*bend == '-') {
				/* We need to verify last -- */
				if (bend > p + 1 && *(bend - 1) == '-') {
					closing = TRUE;
					bend --;
					blen -= 2;
				}
				else {
					/* Not a closing boundary somehow */
					bend ++;
				}
			}
			else {
				bend ++;
			}

			if (*bend == '\r') {
				bend ++;

				/* \r\n */
				if (*bend == '\n') {
					bend ++;
				}
			}
			else {
				/* \n */
				bend ++;
			}

			b.boundary = p - st->start - 3;
			b.start = bend - st->start;

			if (closing) {
				lc_copy = g_malloc (blen + 2);
				memcpy (lc_copy, p, blen + 2);
				rspamd_str_lc (lc_copy, blen + 2);
			}
			else {
				lc_copy = g_malloc (blen);
				memcpy (lc_copy, p, blen);
				rspamd_str_lc (lc_copy, blen);
			}

			rspamd_cryptobox_siphash ((guchar *)&b.hash, lc_copy, blen,
					lib_ctx->hkey);

			if (closing) {
				b.flags = RSPAMD_MIME_BOUNDARY_FLAG_CLOSED;
				rspamd_cryptobox_siphash ((guchar *)&b.closed_hash, lc_copy,
						blen + 2,
						lib_ctx->hkey);
			}
			else {
				b.flags = 0;
				b.closed_hash = 0;
			}

			g_free (lc_copy);
			g_array_append_val (st->boundaries, b);
		}
	}

	return 0;
}

static goffset
rspamd_mime_parser_headers_heuristic (GString *input, goffset *body_start)
{
	const gsize default_max_len = 76;
	gsize max_len = MIN (input->len, default_max_len);
	const gchar *p, *end;
	enum {
		st_before_colon = 0,
		st_colon,
		st_spaces_after_colon,
		st_value,
		st_error
	} state = st_before_colon;

	p = input->str;
	end = p + max_len;

	while (p < end) {
		switch (state) {
		case st_before_colon:
			if (G_UNLIKELY (*p == ':')) {
				state = st_colon;
			}
			else if (G_UNLIKELY (!g_ascii_isgraph (*p))) {
				state = st_error;
			}

			p ++;
			break;
		case st_colon:
			if (g_ascii_isspace (*p)) {
				state = st_spaces_after_colon;
			}
			else {
				state = st_value;
			}
			p ++;
			break;
		case st_spaces_after_colon:
			if (!g_ascii_isspace (*p)) {
				state = st_value;
			}
			p ++;
			break;
		case st_value:
			/* We accept any value */
			goto end;
			break;
		case st_error:
			return (-1);
			break;
		}
	}

end:
	if (state == st_value) {
		if (body_start) {
			*body_start = input->len;
		}

		return input->len;
	}

	return (-1);
}

static void
rspamd_mime_preprocess_message (struct rspamd_task *task,
		struct rspamd_mime_part *top,
		struct rspamd_mime_parser_ctx *st)
{

	if (top->raw_data.begin >= st->pos) {
		rspamd_multipattern_lookup (lib_ctx->mp_boundary,
				top->raw_data.begin - 1,
				top->raw_data.len + 1,
				rspamd_mime_preprocess_cb, st, NULL);
	}
	else {
		rspamd_multipattern_lookup (lib_ctx->mp_boundary,
				st->pos,
				st->end - st->pos,
				rspamd_mime_preprocess_cb, st, NULL);
	}
}

static gboolean
rspamd_mime_parse_message (struct rspamd_task *task,
		struct rspamd_mime_part *part,
		struct rspamd_mime_parser_ctx *st,
		GError **err)
{
	struct rspamd_content_type *ct, *sel = NULL;
	struct rspamd_mime_header *hdr;
	GPtrArray *hdrs = NULL;
	const gchar *pbegin, *p;
	gsize plen, len;
	struct rspamd_mime_part *npart;
	goffset hdr_pos, body_pos;
	guint i;
	gboolean ret = FALSE;
	GString str;

	if (st->stack->len > max_nested) {
		g_set_error (err, RSPAMD_MIME_QUARK, E2BIG, "Nesting level is too high: %d",
				st->stack->len);
		return FALSE;
	}

	/* Allocate real part */
	npart = rspamd_mempool_alloc0 (task->task_pool,
			sizeof (struct rspamd_mime_part));

	if (part == NULL) {
		/* Top level message */
		p = task->msg.begin;
		len = task->msg.len;
		/* Skip any space characters to avoid some bad messages to be unparsed */
		while (len > 0 && g_ascii_isspace (*p)) {
			p ++;
			len --;
		}

		/*
		 * Exim somehow uses mailbox format for messages being scanned:
		 * From x@x.com Fri May 13 19:08:48 2016
		 *
		 * So we check if a task has non-http format then we check for such a line
		 * at the beginning to avoid errors
		 */
		if (!(task->flags & RSPAMD_TASK_FLAG_JSON) || (task->flags &
				RSPAMD_TASK_FLAG_LOCAL_CLIENT)) {
			if (len > sizeof ("From ") - 1) {
				if (memcmp (p, "From ", sizeof ("From ") - 1) == 0) {
					/* Skip to CRLF */
					msg_info_task ("mailbox input detected, enable workaround");
					p += sizeof ("From ") - 1;
					len -= sizeof ("From ") - 1;

					while (len > 0 && *p != '\n') {
						p ++;
						len --;
					}
					while (len > 0 && g_ascii_isspace (*p)) {
						p ++;
						len --;
					}
				}
			}
		}

		str.str = (gchar *)p;
		str.len = len;

		hdr_pos = rspamd_string_find_eoh (&str, &body_pos);

		if (hdr_pos > 0 && hdr_pos < str.len) {

			task->raw_headers_content.begin = (gchar *) (str.str);
			task->raw_headers_content.len = hdr_pos;
			task->raw_headers_content.body_start = str.str + body_pos;

			if (task->raw_headers_content.len > 0) {
				rspamd_mime_headers_process (task, task->raw_headers,
						task->raw_headers_content.begin,
						task->raw_headers_content.len,
						TRUE);
			}

			hdrs = rspamd_message_get_header_from_hash (task->raw_headers,
					task->task_pool,
					"Content-Type", FALSE);
		}
		else {
			/* First apply heuristic, maybe we have just headers */
			hdr_pos = rspamd_mime_parser_headers_heuristic (&str, &body_pos);

			if (hdr_pos > 0 && hdr_pos <= str.len) {
				task->raw_headers_content.begin = (gchar *) (str.str);
				task->raw_headers_content.len = hdr_pos;
				task->raw_headers_content.body_start = str.str + body_pos;

				if (task->raw_headers_content.len > 0) {
					rspamd_mime_headers_process (task, task->raw_headers,
							task->raw_headers_content.begin,
							task->raw_headers_content.len,
							TRUE);
				}

				hdrs = rspamd_message_get_header_from_hash (task->raw_headers,
						task->task_pool,
						"Content-Type", FALSE);
				task->flags |= RSPAMD_TASK_FLAG_BROKEN_HEADERS;
			}
			else {
				body_pos = 0;
			}
		}

		pbegin = st->start + body_pos;
		plen = st->end - pbegin;
		npart->raw_headers = g_hash_table_ref (task->raw_headers);
	}
	else {
		str.str = (gchar *)part->parsed_data.begin;
		str.len = part->parsed_data.len;

		hdr_pos = rspamd_string_find_eoh (&str, &body_pos);
		npart->raw_headers =  g_hash_table_new_full (rspamd_strcase_hash,
				rspamd_strcase_equal, NULL, rspamd_ptr_array_free_hard);

		if (hdr_pos > 0 && hdr_pos < str.len) {
			npart->raw_headers_str = (gchar *) (str.str);
			npart->raw_headers_len = hdr_pos;
			npart->raw_data.begin = str.str + body_pos;

			if (npart->raw_headers_len > 0) {
				rspamd_mime_headers_process (task, npart->raw_headers,
						npart->raw_headers_str,
						npart->raw_headers_len,
						FALSE);
			}
		}

		pbegin = part->parsed_data.begin;
		plen = part->parsed_data.len;

		hdrs = rspamd_message_get_header_from_hash (npart->raw_headers,
				task->task_pool,
				"Content-Type", FALSE);
	}

	npart->raw_data.begin = pbegin;
	npart->raw_data.len = plen;
	npart->parent_part = part;

	if (hdrs == NULL) {
		sel = NULL;
	}
	else {

		for (i = 0; i < hdrs->len; i ++) {
			hdr = g_ptr_array_index (hdrs, i);
			ct = rspamd_content_type_parse (hdr->value, strlen (hdr->value),
					task->task_pool);

			/* Here we prefer multipart content-type or any content-type */
			if (ct) {
				if (sel == NULL) {
					sel = ct;
				}
				else if (ct->flags & RSPAMD_CONTENT_TYPE_MULTIPART) {
					sel = ct;
				}
			}
		}
	}

	if (sel == NULL) {
		/* For messages we automatically assume plaintext */
		msg_info_task ("cannot find content-type for a message, assume text/plain");
		sel = rspamd_mempool_alloc0 (task->task_pool, sizeof (*sel));
		sel->flags = RSPAMD_CONTENT_TYPE_TEXT|RSPAMD_CONTENT_TYPE_MISSING;
		RSPAMD_FTOK_ASSIGN (&sel->type, "text");
		RSPAMD_FTOK_ASSIGN (&sel->subtype, "plain");
	}

	npart->ct = sel;

	if (part == NULL &&
			(sel->flags & (RSPAMD_CONTENT_TYPE_MULTIPART|RSPAMD_CONTENT_TYPE_MESSAGE))) {
		/* Not a trivial message, need to preprocess */
		rspamd_mime_preprocess_message (task, npart, st);
	}

	if (sel->flags & RSPAMD_CONTENT_TYPE_MULTIPART) {
		g_ptr_array_add (st->stack, npart);
		ret = rspamd_mime_parse_multipart_part (task, npart, st, err);
	}
	else if (sel->flags & RSPAMD_CONTENT_TYPE_MESSAGE) {
		g_ptr_array_add (st->stack, npart);
		ret = rspamd_mime_parse_message (task, npart, st, err);
	}
	else {
		ret = rspamd_mime_parse_normal_part (task, npart, st, err);
	}

	if (part) {
		/* Remove message part from the stack */
		g_ptr_array_remove_index_fast (st->stack, st->stack->len - 1);
	}

	return ret;
}

static void
rspamd_mime_parse_stack_free (struct rspamd_mime_parser_ctx *st)
{
	if (st) {
		g_ptr_array_free (st->stack, TRUE);
		g_array_free (st->boundaries, TRUE);
		g_slice_free1 (sizeof (*st), st);
	}
}

gboolean
rspamd_mime_parse_task (struct rspamd_task *task, GError **err)
{
	struct rspamd_mime_parser_ctx *st;
	gboolean ret;

	if (lib_ctx == NULL) {
		rspamd_mime_parser_init_lib ();
	}

	if (++lib_ctx->key_usages > max_key_usages) {
		/* Regenerate siphash key */
		ottery_rand_bytes (lib_ctx->hkey, sizeof (lib_ctx->hkey));
		lib_ctx->key_usages = 0;
	}

	st = g_slice_alloc0 (sizeof (*st));
	st->stack = g_ptr_array_sized_new (4);
	st->pos = task->raw_headers_content.body_start;
	st->end = task->msg.begin + task->msg.len;
	st->boundaries = g_array_sized_new (FALSE, FALSE,
			sizeof (struct rspamd_mime_boundary), 8);

	if (st->pos == NULL) {
		st->pos = task->msg.begin;
	}

	st->start = task->msg.begin;
	ret = rspamd_mime_parse_message (task, NULL, st, err);
	rspamd_mime_parse_stack_free (st);

	return ret;
}
