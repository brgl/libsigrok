/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2015 Bartosz Golaszewski <bgolaszewski@baylibre.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <string.h>
#include "libsigrok.h"
#include "libsigrok-internal.h"

#define LOG_PREFIX "transform/avg"

struct channel_context {
	struct sr_channel *ch;
	uint64_t samples_done;
	float avg_val;
	gboolean stop_avg;
};

struct avg_context {
	uint64_t num_samples;
	GPtrArray *ch_ctx;
	gboolean inframe;
};

static void free_ch_context(gpointer data)
{
	g_free(data);
}

static int init(struct sr_transform *t, GHashTable *options)
{
	struct avg_context *ctx;
	struct sr_channel *ch;
	struct channel_context *ch_ctx;
	GSList *l;

	if (!t || !t->sdi || !options)
		return SR_ERR_ARG;

	t->priv = ctx = g_malloc0(sizeof(struct avg_context));

	ctx->num_samples = g_variant_get_uint64(
				g_hash_table_lookup(options, "samples"));

	ctx->ch_ctx = g_ptr_array_new_full(g_slist_length(t->sdi->channels),
					free_ch_context);

	for (l = t->sdi->channels; l; l = l->next) {
		ch = l->data;
		if (ch->type != SR_CHANNEL_ANALOG || !ch->enabled)
			continue;
		ch_ctx = g_malloc0(sizeof(struct channel_context));
		ch_ctx->ch = ch;
		g_ptr_array_add(ctx->ch_ctx, ch_ctx);
	}

	return SR_OK;
}

static int receive(const struct sr_transform *t,
		struct sr_datafeed_packet *packet_in,
		struct sr_datafeed_packet **packet_out)
{
	struct sr_datafeed_analog *analog;
	unsigned int numch, nums, i, j, s;
	struct avg_context *ctx;
	struct channel_context *ch_ctx;
	int ret = SR_OK;
	GSList *l;
	gboolean do_send = FALSE;

	if (!t || !t->sdi || !packet_in || !packet_out)
		return SR_ERR_ARG;
	ctx = t->priv;

	switch (packet_in->type) {
	case SR_DF_FRAME_BEGIN:
		/*
		 * Make sure to only output the FRAME_BEGIN once
		 * per averaging cycle.
		 */
		if (ctx->inframe) {
			*packet_out = NULL;
			ret = SR_OK_CONTINUE;
		} else {
			ctx->inframe = TRUE;
			*packet_out = packet_in;
		}
		break;
	case SR_DF_ANALOG:
		analog = (struct sr_datafeed_analog *)packet_in->payload;

		if (ctx->num_samples == 0) {
			/* 0 means we're not doing any averaging. */
			*packet_out = packet_in;
			break;
		}

		numch = g_slist_length(analog->channels);
		if ((unsigned int)analog->num_samples > numch)
			nums = analog->num_samples / numch;
		else
			nums = 1;

		s = 0;
		for (i = 0; i < nums; i++) {
			l = analog->channels;
			for (j = 0; j < ctx->ch_ctx->len; j++) {
				ch_ctx = ctx->ch_ctx->pdata[j];
				if (ch_ctx->ch == l->data) {
					if (ch_ctx->stop_avg) {
						continue;
					} else if (ch_ctx->samples_done == 0) {
						ch_ctx->avg_val = analog->data[s];
					} else {
						ch_ctx->avg_val = (ch_ctx->avg_val
								+ analog->data[s]) / 2;
					}

					ch_ctx->samples_done++;
					if (ch_ctx->samples_done >= ctx->num_samples) {
						analog->data[s] = ch_ctx->avg_val;
						ch_ctx->avg_val = 0.0;
						ch_ctx->samples_done = 0;
						ch_ctx->stop_avg = TRUE;
						/*
						 * Pass on packet if at least one
						 * channel reached it's samples
						 * limit.
						 *
						 * XXX This will
						 */
						do_send = TRUE;
					}

					s++;
				}
			}
		}

		if (do_send) {
			for (i = 0; i < ctx->ch_ctx->len; i++) {
				ch_ctx = ctx->ch_ctx->pdata[i];
				ch_ctx->stop_avg = FALSE;
			}
			analog->num_samples = numch;
			*packet_out = packet_in;
			ctx->inframe = FALSE;
		} else {
			*packet_out = NULL;
			ret = SR_OK_CONTINUE;
		}

		break;
	default:
		sr_spew("Unsupported packet type %d, ignoring.", packet_in->type);
		break;
	}

	return ret;
}

static int cleanup(struct sr_transform *t)
{
	struct avg_context *ctx;

	if (!t || !t->sdi)
		return SR_ERR_ARG;
	ctx = t->priv;

	g_ptr_array_free(ctx->ch_ctx, TRUE);
	g_free(ctx);
	t->priv = NULL;

	return SR_OK;
}

static struct sr_option options[] = {
	{ "samples", "Samples", "Number of samples to average over", NULL, NULL },
	ALL_ZERO
};

static const struct sr_option *get_options(void)
{
	/* Averaging is off by default. */
	if (!options[0].def)
		options[0].def = g_variant_ref_sink(g_variant_new_uint64(0));

	return options;
}

SR_PRIV struct sr_transform_module transform_avg = {
	.id = "avg",
	.name = "Averaging",
	.desc = "Average samples before passing them to the output module",
	.options = get_options,
	.init = init,
	.receive = receive,
	.cleanup = cleanup,
};
