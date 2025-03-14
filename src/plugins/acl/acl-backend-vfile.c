/* Copyright (c) 2006-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "istream.h"
#include "nfs-workarounds.h"
#include "settings.h"
#include "mailbox-list-private.h"
#include "acl-global-file.h"
#include "acl-cache.h"
#include "acl-backend-vfile.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define ACL_ESTALE_RETRY_COUNT NFS_ESTALE_RETRY_COUNT
#define ACL_VFILE_DEFAULT_CACHE_SECS 30

static struct acl_backend *acl_backend_vfile_alloc(void)
{
	struct acl_backend_vfile *backend;
	pool_t pool;

	pool = pool_alloconly_create("ACL backend", 512);
	backend = p_new(pool, struct acl_backend_vfile, 1);
	backend->backend.pool = pool;
	return &backend->backend;
}

static int
acl_backend_vfile_init(struct acl_backend *_backend, const char **error_r)
{
	struct event *event = _backend->event;
	struct stat st;

	const char *global_path = _backend->set->acl_global_path;

	if (*global_path != '\0') {
		if (stat(global_path, &st) < 0) {
			*error_r = t_strdup_printf("stat(%s) failed: %m", global_path);
			return -1;
		} else if (S_ISDIR(st.st_mode)) {
			*error_r = t_strdup_printf("Global ACL directories are no longer supported");
			return -1;
		} else {
			_backend->global_file =	acl_global_file_init(
				global_path, _backend->set->acl_cache_ttl / 1000, event);
			e_debug(event, "vfile: Deprecated Global ACL file: %s", global_path);
		}
	}

	_backend->cache =
		acl_cache_init(_backend,
			       sizeof(struct acl_backend_vfile_validity));
	return 0;
}

static void acl_backend_vfile_deinit(struct acl_backend *_backend)
{
	struct acl_backend_vfile *backend =
		container_of(_backend, struct acl_backend_vfile, backend);

	if (backend->acllist_pool != NULL) {
		array_free(&backend->acllist);
		pool_unref(&backend->acllist_pool);
	}
	if (_backend->global_file != NULL)
		acl_global_file_deinit(&_backend->global_file);
	pool_unref(&backend->backend.pool);
}

static const char *
acl_backend_vfile_get_local_dir(struct acl_backend *backend,
				const char *name, const char *vname)
{
	struct mail_namespace *ns = mailbox_list_get_namespace(backend->list);
	struct mailbox_list *list = ns->list;
	struct mail_storage *storage;
	enum mailbox_list_path_type type;
	const char *dir, *inbox;

	if (*name == '\0')
		name = NULL;

	if (backend->set->acl_globals_only)
		return NULL;

	/* ACL files are very important. try to keep them among the main
	   mail files. that's not possible though with a) if the mailbox is
	   a file or b) if the mailbox path doesn't point to filesystem. */
	if (mailbox_list_get_storage(&list, &vname, 0, &storage) < 0)
		return NULL;
	i_assert(list == ns->list);

	type = mail_storage_get_acl_list_path_type(storage);
	if (name == NULL) {
		if (!mailbox_list_get_root_path(list, type, &dir))
			return NULL;
	} else {
		if (mailbox_list_get_path(list, name, type, &dir) <= 0)
			return NULL;
	}

	/* verify that the directory isn't same as INBOX's directory.
	   this is mainly for Maildir. */
	if (name == NULL &&
	    mailbox_list_get_path(list, "INBOX",
				  MAILBOX_LIST_PATH_TYPE_MAILBOX, &inbox) > 0 &&
	    strcmp(inbox, dir) == 0) {
		/* can't have default ACLs with this setup */
		return NULL;
	}
	return dir;
}

static struct acl_object *
acl_backend_vfile_object_init(struct acl_backend *_backend,
			      const char *name)
{
	struct acl_object_vfile *aclobj;
	const char *dir, *vname, *error;

	aclobj = i_new(struct acl_object_vfile, 1);
	aclobj->aclobj.backend = _backend;
	aclobj->aclobj.name = i_strdup(name);

	T_BEGIN {
		if (*name == '\0' ||
		    mailbox_list_is_valid_name(_backend->list, name, &error)) {
			vname = *name == '\0' ? "" :
				mailbox_list_get_vname(_backend->list, name);

			dir = acl_backend_vfile_get_local_dir(_backend, name, vname);
			aclobj->local_path = dir == NULL ? NULL :
				i_strconcat(dir, "/"ACL_FILENAME, NULL);
		} else {
			/* Invalid mailbox name, just use the default
			   global ACL files */
		}
	} T_END;
	return &aclobj->aclobj;
}

static const char *
get_parent_mailbox(struct acl_backend *backend, const char *name)
{
	const char *p;

	p = strrchr(name, mailbox_list_get_hierarchy_sep(backend->list));
	return p == NULL ? NULL : t_strdup_until(name, p);
}

static bool
acl_backend_vfile_has_acl(struct acl_backend *_backend, const char *name)
{
	struct acl_backend_vfile_validity *old_validity, new_validity;
	const char *vname;
	int ret;

	old_validity = acl_cache_get_validity(_backend->cache, name);
	if (old_validity != NULL)
		new_validity = *old_validity;
	else
		i_zero(&new_validity);

	/* The caller wants to stop whenever a parent mailbox exists, even if
	   it has no ACL file. Also, if a mailbox doesn't exist then it can't
	   have a local ACL file. First check if there's a matching global ACL.
	   If not, check if the mailbox exists. */
	vname = *name == '\0' ? "" :
		mailbox_list_get_vname(_backend->list, name);
	struct mailbox *box =
		mailbox_alloc(_backend->list, vname,
			      MAILBOX_FLAG_READONLY | MAILBOX_FLAG_IGNORE_ACLS);
	if (_backend->global_file != NULL) {
		/* check global ACL file */
		ret = acl_global_file_refresh(_backend->global_file);
		if (ret == 0 && acl_global_file_have_any(_backend->global_file, box->vname))
			ret = 1;
	} else {
		/* global ACLs disabled */
		ret = 0;
	}

	if (ret != 0) {
		/* error / global ACL found */
	} else if (mailbox_open(box) == 0) {
		/* mailbox exists */
		ret = 1;
	} else {
		enum mail_error error;
		const char *errstr =
			mailbox_get_last_internal_error(box, &error);
		if (error == MAIL_ERROR_NOTFOUND)
			ret = 0;
		else {
			e_error(box->event, "acl: Failed to open mailbox: %s",
				errstr);
			ret = -1;
		}
	}

	acl_cache_set_validity(_backend->cache, name, &new_validity);
	mailbox_free(&box);
	return ret > 0;
}

static struct acl_object *
acl_backend_vfile_object_init_parent(struct acl_backend *backend,
				     const char *child_name)
{
	const char *parent;

	/* stop at the first parent that
	   a) has global ACL file
	   b) has local ACL file
	   c) exists */
	while ((parent = get_parent_mailbox(backend, child_name)) != NULL) {
		if (acl_backend_vfile_has_acl(backend, parent))
			break;
		child_name = parent;
	}
	if (parent == NULL) {
		/* use the root */
		parent = acl_backend_get_default_object(backend)->name;
	}
	return acl_backend_vfile_object_init(backend, parent);
}

static void acl_backend_vfile_object_deinit(struct acl_object *_aclobj)
{
	struct acl_object_vfile *aclobj = (struct acl_object_vfile *)_aclobj;

	i_free(aclobj->local_path);

	if (array_is_created(&aclobj->aclobj.rights))
		array_free(&aclobj->aclobj.rights);
	pool_unref(&aclobj->aclobj.rights_pool);
	i_free(aclobj->aclobj.name);
	i_free(aclobj);
}

static int
acl_backend_vfile_read(struct acl_object *aclobj, const char *path,
		       struct acl_vfile_validity *validity, bool try_retry)
{
	struct event *event = aclobj->backend->event;
	struct istream *input;
	struct stat st;
	struct acl_rights rights;
	const char *line, *error;
	unsigned int linenum;
	int fd, ret = 0;

	fd = nfs_safe_open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT || errno == ENOTDIR) {
			e_debug(event, "acl vfile: file %s not found", path);
			validity->last_mtime = ACL_VFILE_VALIDITY_MTIME_NOTFOUND;
		} else if (ENOACCESS(errno)) {
			e_debug(event, "acl vfile: no access to file %s", path);

			acl_object_remove_all_access(aclobj);
			validity->last_mtime = ACL_VFILE_VALIDITY_MTIME_NOACCESS;
		} else {
			e_error(event, "open(%s) failed: %m", path);
			return -1;
		}

		validity->last_size = 0;
		validity->last_read_time = ioloop_time;
		return 1;
	}

	if (fstat(fd, &st) < 0) {
		if (errno == ESTALE && try_retry) {
			i_close_fd(&fd);
			return 0;
		}

		e_error(event, "fstat(%s) failed: %m", path);
		i_close_fd(&fd);
		return -1;
	}

	e_debug(event, "acl vfile: reading file %s", path);

	input = i_stream_create_fd(fd, SIZE_MAX);
	i_stream_set_return_partial_line(input, TRUE);
	linenum = 0;
	while ((line = i_stream_read_next_line(input)) != NULL) {
		linenum++;
		if (line[0] == '\0' || line[0] == '#')
			continue;
		T_BEGIN {
			ret = acl_rights_parse_line(line, aclobj->rights_pool,
						    &rights, &error);
			if (ret < 0) {
				e_error(event, "ACL file %s line %u: %s",
					path, linenum, error);
			} else {
				array_push_back(&aclobj->rights, &rights);
			}
		} T_END;
		if (ret < 0)
			break;
	}

	if (ret < 0) {
		/* parsing failure */
	} else if (input->stream_errno != 0) {
		if (input->stream_errno == ESTALE && try_retry)
			ret = 0;
		else {
			ret = -1;
			e_error(event, "read(%s) failed: %s", path,
				i_stream_get_error(input));
		}
	} else {
		if (fstat(fd, &st) < 0) {
			if (errno == ESTALE && try_retry)
				ret = 0;
			else {
				ret = -1;
				e_error(event, "fstat(%s) failed: %m", path);
			}
		} else {
			ret = 1;
			validity->last_read_time = ioloop_time;
			validity->last_mtime = st.st_mtime;
			validity->last_size = st.st_size;
		}
	}

	i_stream_unref(&input);
	if (close(fd) < 0) {
		if (errno == ESTALE && try_retry)
			return 0;

		e_error(event, "close(%s) failed: %m", path);
		return -1;
	}
	return ret;
}

static int
acl_backend_vfile_read_with_retry(struct acl_object *aclobj,
				  const char *path,
				  struct acl_vfile_validity *validity)
{
	unsigned int i;
	int ret;

	if (path == NULL)
		return 0;

	for (i = 0;; i++) {
		ret = acl_backend_vfile_read(aclobj, path, validity,
					     i < ACL_ESTALE_RETRY_COUNT);
		if (ret != 0)
			break;

		/* ESTALE - try again */
	}

	return ret <= 0 ? -1 : 0;
}

static bool
acl_vfile_validity_has_changed(struct acl_backend_vfile *backend,
			       const struct acl_vfile_validity *validity,
			       const struct stat *st)
{
	if (st->st_mtime == validity->last_mtime &&
	    st->st_size == validity->last_size) {
		/* same timestamp, but if it was modified within the
		   same second we want to refresh it again later (but
		   do it only after a couple of seconds so we don't
		   keep re-reading it all the time within those
		   seconds) */
		time_t cache_secs = backend->cache_secs;

		if (validity->last_read_time != 0 &&
		    (st->st_mtime < validity->last_read_time - cache_secs ||
		     ioloop_time - validity->last_read_time <= cache_secs))
			return FALSE;
	}
	return TRUE;
}

static int
acl_backend_vfile_refresh(struct acl_object *aclobj, const char *path,
			  struct acl_vfile_validity *validity)
{
	struct acl_backend_vfile *backend =
		container_of(aclobj->backend, struct acl_backend_vfile, backend);
	struct event *event = backend->backend.event;
	struct stat st;
	int ret;

	if (validity == NULL)
		return 1;
	if (path == NULL ||
	    validity->last_check + (time_t)backend->cache_secs > ioloop_time)
		return 0;

	validity->last_check = ioloop_time;
	ret = stat(path, &st);
	if (ret < 0) {
		if (errno == ENOENT || errno == ENOTDIR) {
			/* if the file used to exist, we have to re-read it */
			return validity->last_mtime != ACL_VFILE_VALIDITY_MTIME_NOTFOUND ? 1 : 0;
		}
		if (ENOACCESS(errno))
			return validity->last_mtime != ACL_VFILE_VALIDITY_MTIME_NOACCESS ? 1 : 0;
		e_error(event, "stat(%s) failed: %m", path);
		return -1;
	}
	return acl_vfile_validity_has_changed(backend, validity, &st) ? 1 : 0;
}

int acl_backend_vfile_object_get_mtime(struct acl_object *aclobj,
				       time_t *mtime_r)
{
	struct acl_backend_vfile_validity *validity;

	validity = acl_cache_get_validity(aclobj->backend->cache, aclobj->name);
	if (validity == NULL)
		return -1;

	if (validity->local_validity.last_mtime != 0)
		*mtime_r = validity->local_validity.last_mtime;
	else if (validity->global_validity.last_mtime != 0)
		*mtime_r = validity->global_validity.last_mtime;
	else
		*mtime_r = 0;
	return 0;
}

static int
acl_backend_global_file_refresh(struct acl_object *_aclobj,
				struct acl_vfile_validity *validity)
{
	struct acl_backend_vfile *backend =
		container_of(_aclobj->backend, struct acl_backend_vfile, backend);
	struct stat st;

	if (acl_global_file_refresh(_aclobj->backend->global_file) < 0)
		return -1;

	acl_global_file_last_stat(_aclobj->backend->global_file, &st);
	if (validity == NULL)
		return 1;
	return acl_vfile_validity_has_changed(backend, validity, &st) ? 1 : 0;
}

static int acl_backend_vfile_object_refresh_cache(struct acl_object *_aclobj)
{
	struct acl_object_vfile *aclobj =
		container_of(_aclobj, struct acl_object_vfile, aclobj);
	struct acl_backend_vfile *backend =
		container_of(_aclobj->backend, struct acl_backend_vfile, backend);
	struct acl_backend_vfile_validity *old_validity;
	struct acl_backend_vfile_validity validity;
	time_t mtime;
	int ret = 0;

	old_validity = acl_cache_get_validity(_aclobj->backend->cache,
					      _aclobj->name);
	if (_aclobj->backend->global_file != NULL)
		ret = acl_backend_global_file_refresh(_aclobj, old_validity == NULL ? NULL :
						      &old_validity->global_validity);
	if (ret == 0) {
		ret = acl_backend_vfile_refresh(_aclobj, aclobj->local_path,
						old_validity == NULL ? NULL :
						&old_validity->local_validity);
	}
	if (ret <= 0)
		return ret;

	/* either global or local ACLs changed, need to re-read both */
	if (!array_is_created(&_aclobj->rights)) {
		_aclobj->rights_pool =
			pool_alloconly_create("acl rights", 256);
		i_array_init(&_aclobj->rights, 16);
	} else {
		array_clear(&_aclobj->rights);
		p_clear(_aclobj->rights_pool);
	}

	i_zero(&validity);
	if (_aclobj->backend->global_file != NULL) {
		struct stat st;

		acl_object_add_global_acls(_aclobj);
		acl_global_file_last_stat(_aclobj->backend->global_file, &st);
		validity.global_validity.last_read_time = ioloop_time;
		validity.global_validity.last_mtime = st.st_mtime;
		validity.global_validity.last_size = st.st_size;
	}

	if (acl_backend_get_mailbox_acl(_aclobj->backend, _aclobj) < 0)
		return -1;

	if (acl_backend_vfile_read_with_retry(_aclobj, aclobj->local_path,
					      &validity.local_validity) < 0)
		return -1;

	acl_rights_sort(_aclobj);
	/* update cache only after we've successfully read everything */
	acl_object_rebuild_cache(_aclobj);
	acl_cache_set_validity(_aclobj->backend->cache,
			       _aclobj->name, &validity);

	if (acl_backend_vfile_object_get_mtime(_aclobj, &mtime) == 0)
		acl_backend_vfile_acllist_verify(backend, _aclobj->name, mtime);
	return 0;
}

static int acl_backend_vfile_object_last_changed(struct acl_object *_aclobj,
						 time_t *last_changed_r)
{
	struct acl_backend_vfile_validity *old_validity;

	*last_changed_r = 0;

	old_validity = acl_cache_get_validity(_aclobj->backend->cache,
					      _aclobj->name);
	if (old_validity == NULL) {
		if (acl_backend_vfile_object_refresh_cache(_aclobj) < 0)
			return -1;
		old_validity = acl_cache_get_validity(_aclobj->backend->cache,
						      _aclobj->name);
		if (old_validity == NULL)
			return 0;
	}
	*last_changed_r = old_validity->local_validity.last_mtime;
	return 0;
}

const struct acl_backend_vfuncs acl_backend_vfile = {
	.name = "vfile",
	.alloc = acl_backend_vfile_alloc,
	.init = acl_backend_vfile_init,
	.deinit = acl_backend_vfile_deinit,
	.nonowner_lookups_iter_init = acl_backend_vfile_nonowner_iter_init,
	.nonowner_lookups_iter_next = acl_backend_vfile_nonowner_iter_next,
	.nonowner_lookups_iter_deinit = acl_backend_vfile_nonowner_iter_deinit,
	.nonowner_lookups_rebuild = acl_backend_vfile_nonowner_lookups_rebuild,
	.object_init = acl_backend_vfile_object_init,
	.object_init_parent = acl_backend_vfile_object_init_parent,
	.object_deinit = acl_backend_vfile_object_deinit,
	.object_refresh_cache = acl_backend_vfile_object_refresh_cache,
	.object_update = acl_backend_vfile_object_update,
	.last_changed = acl_backend_vfile_object_last_changed,
	.object_list_init = acl_default_object_list_init,
	.object_list_next = acl_default_object_list_next,
	.object_list_deinit = acl_default_object_list_deinit
};
