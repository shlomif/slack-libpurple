#include <errno.h>

#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-rtm.h"
#include "slack-blist.h"
#include "slack-message.h"
#include "slack-user.h"
#include "slack-channel.h"
#include "slack-im.h"

static void slack_presence_sub(SlackAccount *sa) {
	GString *ids = g_string_new("[");
	GHashTableIter iter;
	gpointer id;
	SlackUser *user;
	g_hash_table_iter_init(&iter, sa->ims);
	gboolean first = TRUE;
	while (g_hash_table_iter_next(&iter, &id, (gpointer*)&user)) {
		if (first)
			first = FALSE;
		else
			g_string_append_c(ids, ',');
		append_json_string(ids, user->object.id);
	}
	g_string_append_c(ids, ']');

	slack_rtm_send(sa, NULL, NULL, "presence_sub", "ids", ids->str, NULL);
	g_string_free(ids, TRUE);
}

static gboolean im_update(SlackAccount *sa, json_value *json, const json_value *open_user) {
	const char *sid = json_get_strptr(json);
	if (sid)
		json = NULL;
	else
		sid = json_get_prop_strptr(json, "id");
	if (!sid)
		return FALSE;
	slack_object_id id;
	slack_object_id_set(id, sid);

	SlackUser *user = g_hash_table_lookup(sa->ims, id);

	if (!json_get_prop_boolean(json, "is_open", open_user != NULL)) {
		if (!user)
			return FALSE;
		g_return_val_if_fail(*user->im, FALSE);
		/* Don't forget im ids because apparently slack might still use them? (#22)
		g_hash_table_remove(sa->ims, user->im);
		slack_object_id_clear(user->im);
		*/
		if (user->buddy) {
			slack_blist_uncache(sa, &user->buddy->node);
			purple_blist_remove_buddy(user->buddy);
			user->buddy = NULL;
		}
		return TRUE;
	}

	gboolean changed = FALSE;

	const char *user_id = json_get_prop_strptr(json, "user") ?: json_get_strptr(open_user);
	g_return_val_if_fail(user_id, FALSE);

	if (!user) {
		user = (SlackUser *)slack_object_hash_table_lookup(sa->users, user_id);
		if (!user)
			return FALSE;
		if (slack_object_id_cmp(user->im, id)) {
			if (*user->im)
				g_hash_table_remove(sa->ims, user->im);
			slack_object_id_copy(user->im, id);
			g_hash_table_insert(sa->ims, user->im, user);
			changed = TRUE;
		}
	} else
		g_warn_if_fail(slack_object_id_is(user->object.id, user_id));

	if (!user->buddy) {
		user->buddy = g_hash_table_lookup(sa->buddies, sid);
		if (user->buddy && PURPLE_BLIST_NODE_IS_BUDDY(PURPLE_BLIST_NODE(user->buddy))) {
			if (user->name && strcmp(user->name, purple_buddy_get_name(user->buddy))) {
				purple_blist_rename_buddy(user->buddy, user->name);
				changed = TRUE;
			}
		} else {
			user->buddy = purple_buddy_new(sa->account, user->name, NULL);
			slack_blist_cache(sa, &user->buddy->node, sid);
			purple_blist_add_buddy(user->buddy, NULL, sa->blist, NULL);
			changed = TRUE;
		}
	}

	purple_debug_misc("slack", "im %s: %s\n", user->im, user->object.id);
	return changed;
}

void slack_im_close(SlackAccount *sa, json_value *json) {
	if (im_update(sa, json_get_prop(json, "channel"), NULL))
		slack_presence_sub(sa);
}

void slack_im_open(SlackAccount *sa, json_value *json) {
	if (im_update(sa, json_get_prop(json, "channel"), json_get_prop(json, "user")))
		slack_presence_sub(sa);
}

static void im_list_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json_value *ims = json_get_prop_type(json, "ims", array);
	if (!ims) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error ?: "Missing IM channel list");
		return;
	}

	for (unsigned i = 0; i < ims->u.array.length; i ++)
		im_update(sa, ims->u.array.values[i], &json_value_none);

	char *cursor = json_get_prop_strptr(json_get_prop(json, "response_metadata"), "next_cursor");
	if (cursor && *cursor)
		slack_api_call(sa, im_list_cb, NULL, "im.list", SLACK_PAGINATE_LIMIT, "cursor", cursor, NULL);
	else {
		slack_presence_sub(sa);
		slack_channels_load(sa);
	}
}

void slack_ims_load(SlackAccount *sa) {
	purple_connection_update_progress(sa->gc, "Loading IM channels", 5, SLACK_CONNECT_STEPS);
	g_hash_table_remove_all(sa->ims);
	slack_api_call(sa, im_list_cb, NULL, "im.list", SLACK_PAGINATE_LIMIT, NULL);
}

struct send_im {
	SlackUser *user;
	char *msg;
	PurpleMessageFlags flags;
};

static void send_im_free(struct send_im *send) {
	g_object_unref(send->user);
	g_free(send->msg);
	g_free(send);
}

static void send_im_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct send_im *send = data;

	if (error)
		purple_conv_present_error(send->user->name, sa->account, error);

	send_im_free(send);
}

static void send_im_open_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct send_im *send = data;

	json = json_get_prop_type(json, "channel", object);
	if (json)
		im_update(sa, json, &json_value_none);

	if (error || !*send->user->im) {
		purple_conv_present_error(send->user->name, sa->account, error ?: "failed to open IM channel");
		send_im_free(send);
		return;
	}

	GString *channel = append_json_string(g_string_new(NULL), send->user->im);
	GString *text = append_json_string(g_string_new(NULL), send->msg);
	slack_rtm_send(sa, send_im_cb, send, "message", "channel", channel->str, "text", text->str, NULL);
	g_string_free(channel, TRUE);
	g_string_free(text, TRUE);
}

int slack_send_im(PurpleConnection *gc, const char *who, const char *msg, PurpleMessageFlags flags) {
	SlackAccount *sa = gc->proto_data;

	SlackUser *user = g_hash_table_lookup(sa->user_names, who);
	if (!user)
		return -ENOENT;

	gchar *m = slack_html_to_message(sa, msg, flags);
	glong mlen = g_utf8_strlen(m, 16384);
	if (mlen > 4000)
		return -E2BIG;

	struct send_im *send = g_new(struct send_im, 1);
	send->user = g_object_ref(user);
	send->msg = m;
	send->flags = flags;

	if (!*user->im)
		slack_api_call(sa, send_im_open_cb, send, "im.open", "user", user->object.id, "return_im", "true", NULL);
	else
		send_im_open_cb(sa, send, NULL, NULL);

	return 1;
}
