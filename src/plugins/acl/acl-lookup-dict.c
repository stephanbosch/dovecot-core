/* Copyright (c) 2008-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "dict.h"
#include "settings.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "acl-api-private.h"
#include "acl-storage.h"
#include "acl-plugin.h"
#include "acl-lookup-dict.h"


#define DICT_SHARED_BOXES_PATH "shared-boxes/"

extern struct event_category event_category_acl;

struct acl_lookup_dict {
	struct mail_user *user;
	struct dict *dict;
	struct event *event;
};

struct acl_lookup_dict_iter {
	pool_t pool;
	struct acl_lookup_dict *dict;

	pool_t iter_value_pool;
	ARRAY_TYPE(const_string) iter_ids;
	ARRAY_TYPE(const_string) iter_values;
	unsigned int iter_idx, iter_value_idx;

	bool failed:1;
};

int acl_lookup_dict_init(struct mail_user *user, struct acl_lookup_dict **dict_r,
			 const char **error_r)
{
	struct acl_lookup_dict *dict;

	dict = i_new(struct acl_lookup_dict, 1);
	dict->user = user;
	dict->event = event_create(user->event);
	*dict_r = dict;

	event_add_category(dict->event, &event_category_acl);
	event_set_append_log_prefix(dict->event, "acl: ");
	settings_event_add_filter_name(dict->event, "acl_sharing_map");
	return dict_init_auto(dict->event, &dict->dict, error_r);
}

void acl_lookup_dict_deinit(struct acl_lookup_dict **_dict)
{
	struct acl_lookup_dict *dict = *_dict;

	*_dict = NULL;
	if (dict->dict != NULL)
		dict_deinit(&dict->dict);
	event_unref(&dict->event);
	i_free(dict);
}

bool acl_lookup_dict_is_enabled(struct acl_lookup_dict *dict)
{
	return dict->dict != NULL;
}

static void
acl_lookup_dict_write_rights_id(string_t *dest, const struct acl_rights *right)
{
	switch (right->id_type) {
	case ACL_ID_ANYONE:
	case ACL_ID_AUTHENTICATED:
		/* don't bother separating these */
		str_append(dest, "anyone");
		break;
	case ACL_ID_USER:
		str_append(dest, "user/");
		str_append(dest, right->identifier);
		break;
	case ACL_ID_GROUP:
	case ACL_ID_GROUP_OVERRIDE:
		str_append(dest, "group/");
		str_append(dest, right->identifier);
		break;
	case ACL_ID_OWNER:
	case ACL_ID_TYPE_COUNT:
		i_unreached();
	}
}

static bool
acl_rights_is_same_user(const struct acl_rights *right, struct mail_user *user)
{
	return right->id_type == ACL_ID_USER &&
		strcmp(right->identifier, user->username) == 0;
}

static int acl_lookup_dict_rebuild_add_backend(struct mail_namespace *ns,
					       ARRAY_TYPE(const_string) *ids)
{
	struct acl_backend *backend;
	struct acl_mailbox_list_context *ctx;
	struct acl_object *aclobj;
	struct acl_object_list_iter *iter;
	struct acl_rights rights;
	const char *name, *id_dup;
	string_t *id;
	int ret = 0;

	if ((ns->flags & NAMESPACE_FLAG_NOACL) != 0 || ns->owner == NULL ||
	    ACL_LIST_CONTEXT(ns->list) == NULL)
		return 0;

	backend = acl_mailbox_list_get_backend(ns->list);
	if (backend->set->acl_ignore)
		return 0;

	id = t_str_new(128);
	ctx = acl_backend_nonowner_lookups_iter_init(backend);
	while (acl_backend_nonowner_lookups_iter_next(ctx, &name)) {
		aclobj = acl_object_init_from_name(backend, name);

		iter = acl_object_list_init(aclobj);
		while (acl_object_list_next(iter, &rights)) {
			/* avoid pointless user -> user entries,
			   which some clients do */
			if (acl_rights_has_nonowner_lookup_changes(&rights) &&
			    !acl_rights_is_same_user(&rights, ns->owner)) {
				str_truncate(id, 0);
				acl_lookup_dict_write_rights_id(id, &rights);
				str_append_c(id, '/');
				str_append(id, ns->owner->username);
				id_dup = t_strdup(str_c(id));
				array_push_back(ids, &id_dup);
			}
		}
		if (acl_object_list_deinit(&iter) < 0) ret = -1;
		acl_object_deinit(&aclobj);
	}
	if (acl_backend_nonowner_lookups_iter_deinit(&ctx) < 0) ret = -1;
	return ret;
}

static int
acl_lookup_dict_rebuild_update(struct acl_lookup_dict *dict,
			       const ARRAY_TYPE(const_string) *new_ids_arr,
			       bool no_removes)
{
	struct event *event = dict->event;
	const char *username = dict->user->username;
	struct dict_iterate_context *iter;
	struct dict_transaction_context *dt = NULL;
	const char *prefix, *key, *value, *const *old_ids, *const *new_ids, *p;
	const char *error;
	ARRAY_TYPE(const_string) old_ids_arr;
	unsigned int newi, oldi, old_count, new_count;
	string_t *path;
	size_t prefix_len;
	int ret;
	const struct dict_op_settings *set = mail_user_get_dict_op_settings(dict->user);

	/* get all existing identifiers for the user. we might be able to
	   sync identifiers also for other users whose shared namespaces we
	   have, but it's possible that the other users have other namespaces
	   that aren't visible to us, so we don't want to remove anything
	   that could break them. */
	t_array_init(&old_ids_arr, 128);
	prefix = DICT_PATH_SHARED DICT_SHARED_BOXES_PATH;
	prefix_len = strlen(prefix);
	iter = dict_iterate_init(dict->dict, set, prefix, DICT_ITERATE_FLAG_RECURSE);
	while (dict_iterate(iter, &key, &value)) {
		/* prefix/$type/$dest/$source */
		key += prefix_len;
		p = strrchr(key, '/');
		if (p != NULL && strcmp(p + 1, username) == 0) {
			key = t_strdup_until(key, p);
			array_push_back(&old_ids_arr, &key);
		}
	}
	if (dict_iterate_deinit(&iter, &error) < 0) {
		e_error(event, "dict iteration failed: %s - can't update dict", error);
		return -1;
	}

	/* sort the existing identifiers */
	array_sort(&old_ids_arr, i_strcmp_p);

	/* sync the identifiers */
	path = t_str_new(256);
	str_append(path, prefix);

	old_ids = array_get(&old_ids_arr, &old_count);
	new_ids = array_get(new_ids_arr, &new_count);
	for (newi = oldi = 0; newi < new_count || oldi < old_count; ) {
		ret = newi == new_count ? 1 :
			oldi == old_count ? -1 :
			strcmp(new_ids[newi], old_ids[oldi]);
		if (ret == 0) {
			newi++; oldi++;
		} else if (ret < 0) {
			/* new identifier, add it */
			str_truncate(path, prefix_len);
			str_append(path, new_ids[newi]);
			dt = dict_transaction_begin(dict->dict, set);
			dict_set(dt, str_c(path), "1");
			newi++;
		} else if (!no_removes) {
			/* old identifier removed */
			str_truncate(path, prefix_len);
			str_append(path, old_ids[oldi]);
			str_append_c(path, '/');
			str_append(path, username);
			dt = dict_transaction_begin(dict->dict, set);
			dict_unset(dt, str_c(path));
			oldi++;
		}
		if (dt != NULL && dict_transaction_commit(&dt, &error) < 0) {
			e_error(event, "dict commit failed: %s", error);
			return -1;
		}
		i_assert(dt == NULL);
	}
	return 0;
}

int acl_lookup_dict_rebuild(struct acl_lookup_dict *dict)
{
	struct mail_namespace *ns;
	ARRAY_TYPE(const_string) ids_arr;
	const char **ids;
	unsigned int i, dest, count;
	int ret = 0;

	if (dict->dict == NULL)
		return 0;

	/* get all ACL identifiers with a positive lookup right */
	t_array_init(&ids_arr, 128);
	for (ns = dict->user->namespaces; ns != NULL; ns = ns->next) {
		if (acl_lookup_dict_rebuild_add_backend(ns, &ids_arr) < 0)
			ret = -1;
	}

	/* sort identifiers and remove duplicates */
	array_sort(&ids_arr, i_strcmp_p);

	ids = array_get_modifiable(&ids_arr, &count);
	for (i = 1, dest = 0; i < count; i++) {
		if (strcmp(ids[dest], ids[i]) != 0) {
			if (++dest != i)
				ids[dest] = ids[i];
		}
	}
	if (++dest < count)
		array_delete(&ids_arr, dest, count-dest);

	/* if lookup failed at some point we can still add new ids,
	   but we can't remove any existing ones */
	if (acl_lookup_dict_rebuild_update(dict, &ids_arr, ret < 0) < 0)
		ret = -1;
	return ret;
}

static void acl_lookup_dict_iterate_read(struct acl_lookup_dict_iter *iter)
{
	struct event *event = iter->dict->event;
	struct dict_iterate_context *dict_iter;
	const char *id, *prefix, *key, *value, *error;
	size_t prefix_len;

	id = array_idx_elem(&iter->iter_ids, iter->iter_idx);
	iter->iter_idx++;
	iter->iter_value_idx = 0;

	prefix = t_strconcat(DICT_PATH_SHARED DICT_SHARED_BOXES_PATH,
			     id, "/", NULL);
	prefix_len = strlen(prefix);

	/* read all of it to memory. at least currently dict-proxy can support
	   only one iteration at a time, but the acl code can end up rebuilding
	   the dict, which opens another iteration. */
	p_clear(iter->iter_value_pool);
	array_clear(&iter->iter_values);
	const struct dict_op_settings *set = mail_user_get_dict_op_settings(iter->dict->user);
	dict_iter = dict_iterate_init(iter->dict->dict, set, prefix,
				      DICT_ITERATE_FLAG_RECURSE);
	while (dict_iterate(dict_iter, &key, &value)) {
		i_assert(prefix_len < strlen(key));

		key = p_strdup(iter->iter_value_pool, key + prefix_len);
		array_push_back(&iter->iter_values, &key);
	}
	if (dict_iterate_deinit(&dict_iter, &error) < 0) {
		e_error(event, "%s", error);
		iter->failed = TRUE;
	}
}

struct acl_lookup_dict_iter *
acl_lookup_dict_iterate_visible_init(struct acl_lookup_dict *dict,
				     const ARRAY_TYPE(const_string) *groups)
{
	struct acl_user *auser = ACL_USER_CONTEXT(dict->user);
	struct acl_lookup_dict_iter *iter;
	const char *id;
	pool_t pool;

	i_assert(auser != NULL);

	pool = pool_alloconly_create("acl lookup dict iter", 1024);
	iter = p_new(pool, struct acl_lookup_dict_iter, 1);
	iter->pool = pool;
	iter->dict = dict;

	p_array_init(&iter->iter_ids, pool, 16);
	id = "anyone";
	array_push_back(&iter->iter_ids, &id);
	id = p_strconcat(pool, "user/", dict->user->username, NULL);
	array_push_back(&iter->iter_ids, &id);

	i_array_init(&iter->iter_values, 64);
	iter->iter_value_pool =
		pool_alloconly_create("acl lookup dict iter values", 1024);

	/* get all groups we belong to */
	if (array_is_created(groups)) {
		const char *group;
		array_foreach_elem(groups, group) {
			id = p_strconcat(pool, "group/", group,
					 NULL);
			array_push_back(&iter->iter_ids, &id);
		}
	}

	/* iterate through all identifiers that match us, start with the
	   first one */
	if (dict->dict != NULL)
		acl_lookup_dict_iterate_read(iter);
	else
		array_clear(&iter->iter_ids);
	return iter;
}

const char *
acl_lookup_dict_iterate_visible_next(struct acl_lookup_dict_iter *iter)
{
	const char *const *keys;
	unsigned int count;

	keys = array_get(&iter->iter_values, &count);
	if (iter->iter_value_idx < count)
		return keys[iter->iter_value_idx++];

	if (iter->iter_idx < array_count(&iter->iter_ids)) {
		/* get to the next iterator */
		acl_lookup_dict_iterate_read(iter);
		return acl_lookup_dict_iterate_visible_next(iter);
	}
	return NULL;
}

int acl_lookup_dict_iterate_visible_deinit(struct acl_lookup_dict_iter **_iter)
{
	struct acl_lookup_dict_iter *iter = *_iter;
	int ret = iter->failed ? -1 : 0;

	*_iter = NULL;
	array_free(&iter->iter_values);
	pool_unref(&iter->iter_value_pool);
	pool_unref(&iter->pool);
	return ret;
}
