/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2/config.h"
#include "git2/types.h"
#include "git2/oid.h"

#include "config.h"
#include "repository.h"
#include "remote.h"
#include "fetch.h"
#include "refs.h"
#include "pkt.h"

#include <regex.h>

static int parse_remote_refspec(git_config *cfg, git_refspec *refspec, const char *var, bool is_fetch)
{
	int error;
	const char *val;

	if ((error = git_config_get_string(&val, cfg, var)) < 0)
		return error;

	return git_refspec__parse(refspec, val, is_fetch);
}

static int download_tags_value(git_remote *remote, git_config *cfg)
{
	const char *val;
	git_buf buf = GIT_BUF_INIT;
	int error;

	if (remote->download_tags != GIT_REMOTE_DOWNLOAD_TAGS_UNSET)
		return 0;

	/* This is the default, let's see if we need to change it */
	remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_AUTO;
	if (git_buf_printf(&buf, "remote.%s.tagopt", remote->name) < 0)
		return -1;

	error = git_config_get_string(&val, cfg, git_buf_cstr(&buf));
	git_buf_free(&buf);
	if (!error && !strcmp(val, "--no-tags"))
		remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
	else if (!error && !strcmp(val, "--tags"))
		remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_ALL;

	if (error == GIT_ENOTFOUND)
		error = 0;

	return error;
}

int git_remote_new(git_remote **out, git_repository *repo, const char *name, const char *url, const char *fetch)
{
	git_remote *remote;

	/* name is optional */
	assert(out && repo && url);

	remote = git__malloc(sizeof(git_remote));
	GITERR_CHECK_ALLOC(remote);

	memset(remote, 0x0, sizeof(git_remote));
	remote->repo = repo;
	remote->check_cert = 1;

	if (git_vector_init(&remote->refs, 32, NULL) < 0)
		return -1;

	remote->url = git__strdup(url);
	GITERR_CHECK_ALLOC(remote->url);

	if (name != NULL) {
		remote->name = git__strdup(name);
		GITERR_CHECK_ALLOC(remote->name);
	}

	if (fetch != NULL) {
		if (git_refspec__parse(&remote->fetch, fetch, true) < 0)
			goto on_error;
	}

	/* A remote without a name doesn't download tags */
	if (!name) {
		remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
	}

	*out = remote;
	return 0;

on_error:
	git_remote_free(remote);
	return -1;
}

int git_remote_load(git_remote **out, git_repository *repo, const char *name)
{
	git_remote *remote;
	git_buf buf = GIT_BUF_INIT;
	const char *val;
	int error = 0;
	git_config *config;

	assert(out && repo && name);

	if (git_repository_config__weakptr(&config, repo) < 0)
		return -1;

	remote = git__malloc(sizeof(git_remote));
	GITERR_CHECK_ALLOC(remote);

	memset(remote, 0x0, sizeof(git_remote));
	remote->check_cert = 1;
	remote->name = git__strdup(name);
	GITERR_CHECK_ALLOC(remote->name);

	if (git_vector_init(&remote->refs, 32, NULL) < 0) {
		error = -1;
		goto cleanup;
	}

	if (git_buf_printf(&buf, "remote.%s.url", name) < 0) {
		error = -1;
		goto cleanup;
	}

	if ((error = git_config_get_string(&val, config, git_buf_cstr(&buf))) < 0)
		goto cleanup;

	remote->repo = repo;
	remote->url = git__strdup(val);
	GITERR_CHECK_ALLOC(remote->url);

	git_buf_clear(&buf);
	if (git_buf_printf(&buf, "remote.%s.pushurl", name) < 0) {
		error = -1;
		goto cleanup;
	}

	error = git_config_get_string(&val, config, git_buf_cstr(&buf));
	if (error == GIT_ENOTFOUND)
		error = 0;

	if (error < 0) {
		error = -1;
		goto cleanup;
	}

	if (val) {
		remote->pushurl = git__strdup(val);
		GITERR_CHECK_ALLOC(remote->pushurl);
	}

	git_buf_clear(&buf);
	if (git_buf_printf(&buf, "remote.%s.fetch", name) < 0) {
		error = -1;
		goto cleanup;
	}

	error = parse_remote_refspec(config, &remote->fetch, git_buf_cstr(&buf), true);
	if (error == GIT_ENOTFOUND)
		error = 0;

	if (error < 0) {
		error = -1;
		goto cleanup;
	}

	git_buf_clear(&buf);
	if (git_buf_printf(&buf, "remote.%s.push", name) < 0) {
		error = -1;
		goto cleanup;
	}

	error = parse_remote_refspec(config, &remote->push, git_buf_cstr(&buf), false);
	if (error == GIT_ENOTFOUND)
		error = 0;

	if (error < 0) {
		error = -1;
		goto cleanup;
	}

	if (download_tags_value(remote, config) < 0)
		goto cleanup;

	*out = remote;

cleanup:
	git_buf_free(&buf);

	if (error < 0)
		git_remote_free(remote);

	return error;
}

static int ensure_remote_name_is_valid(const char *name)
{
	git_buf buf = GIT_BUF_INIT;
	git_refspec refspec;
	int error = -1;

	if (!name || *name == '\0')
		goto cleanup;

	git_buf_printf(&buf, "refs/heads/test:refs/remotes/%s/test", name);
	error = git_refspec__parse(&refspec, git_buf_cstr(&buf), true);

	git_buf_free(&buf);
	git_refspec__free(&refspec);

cleanup:
	if (error)
		giterr_set(
			GITERR_CONFIG,
			"'%s' is not a valid remote name.", name);

	return error;
}

static int update_config_refspec(
	git_config *config,
	const char *remote_name,
	const git_refspec *refspec,
	int git_direction)
{
	git_buf name = GIT_BUF_INIT, value = GIT_BUF_INIT;
	int error = -1;

	if (refspec->src == NULL || refspec->dst == NULL)
		return 0;

	if (git_buf_printf(
		&name,
		"remote.%s.%s",
		remote_name,
		git_direction == GIT_DIR_FETCH ? "fetch" : "push") < 0)
			goto cleanup;

	if (git_refspec__serialize(&value, refspec) < 0)
		goto cleanup;

	error = git_config_set_string(
		config,
		git_buf_cstr(&name),
		git_buf_cstr(&value));

cleanup:
	git_buf_free(&name);
	git_buf_free(&value);

	return error;
}

int git_remote_save(const git_remote *remote)
{
	int error;
	git_config *config;
	const char *tagopt = NULL;
	git_buf buf = GIT_BUF_INIT;

	assert(remote);

	if (ensure_remote_name_is_valid(remote->name) < 0)
		return -1;

	if (git_repository_config__weakptr(&config, remote->repo) < 0)
		return -1;

	if (git_buf_printf(&buf, "remote.%s.url", remote->name) < 0)
		return -1;

	if (git_config_set_string(config, git_buf_cstr(&buf), remote->url) < 0) {
		git_buf_free(&buf);
		return -1;
	}

	git_buf_clear(&buf);
	if (git_buf_printf(&buf, "remote.%s.pushurl", remote->name) < 0)
		return -1;

	if (remote->pushurl) {
		if (git_config_set_string(config, git_buf_cstr(&buf), remote->pushurl) < 0) {
			git_buf_free(&buf);
			return -1;
		}
	} else {
		int error = git_config_delete(config, git_buf_cstr(&buf));
		if (error == GIT_ENOTFOUND) {
			error = 0;
		}
		if (error < 0) {
			git_buf_free(&buf);
			return -1;
		}
	}

	if (update_config_refspec(
		config,
		remote->name,
		&remote->fetch,
		GIT_DIR_FETCH) < 0)
			goto on_error;

	if (update_config_refspec(
		config,
		remote->name,
		&remote->push,
		GIT_DIR_PUSH) < 0)
			goto on_error;

	/*
	 * What action to take depends on the old and new values. This
	 * is describes by the table below. tagopt means whether the
	 * is already a value set in the config
	 *
	 *            AUTO     ALL or NONE
	 *         +-----------------------+
	 *  tagopt | remove  |     set     |
	 *         +---------+-------------|
	 * !tagopt | nothing |     set     |
	 *         +---------+-------------+
	 */

	git_buf_clear(&buf);
	if (git_buf_printf(&buf, "remote.%s.tagopt", remote->name) < 0)
		goto on_error;

	error = git_config_get_string(&tagopt, config, git_buf_cstr(&buf));
	if (error < 0 && error != GIT_ENOTFOUND)
		goto on_error;

	if (remote->download_tags == GIT_REMOTE_DOWNLOAD_TAGS_ALL) {
		if (git_config_set_string(config, git_buf_cstr(&buf), "--tags") < 0)
			goto on_error;
	} else if (remote->download_tags == GIT_REMOTE_DOWNLOAD_TAGS_NONE) {
		if (git_config_set_string(config, git_buf_cstr(&buf), "--no-tags") < 0)
			goto on_error;
	} else if (tagopt) {
		if (git_config_delete(config, git_buf_cstr(&buf)) < 0)
			goto on_error;
	}

	git_buf_free(&buf);

	return 0;

on_error:
	git_buf_free(&buf);
	return -1;
}

const char *git_remote_name(git_remote *remote)
{
	assert(remote);
	return remote->name;
}

const char *git_remote_url(git_remote *remote)
{
	assert(remote);
	return remote->url;
}

int git_remote_set_url(git_remote *remote, const char* url)
{
	assert(remote);
	assert(url);

	git__free(remote->url);
	remote->url = git__strdup(url);
	GITERR_CHECK_ALLOC(remote->url);

	return 0;
}

const char *git_remote_pushurl(git_remote *remote)
{
	assert(remote);
	return remote->pushurl;
}

int git_remote_set_pushurl(git_remote *remote, const char* url)
{
	assert(remote);

	git__free(remote->pushurl);
	if (url) {
		remote->pushurl = git__strdup(url);
		GITERR_CHECK_ALLOC(remote->pushurl);
	} else {
		remote->pushurl = NULL;
	}
	return 0;
}

int git_remote_set_fetchspec(git_remote *remote, const char *spec)
{
	git_refspec refspec;

	assert(remote && spec);

	if (git_refspec__parse(&refspec, spec, true) < 0)
		return -1;

	git_refspec__free(&remote->fetch);
	remote->fetch.src = refspec.src;
	remote->fetch.dst = refspec.dst;

	return 0;
}

const git_refspec *git_remote_fetchspec(git_remote *remote)
{
	assert(remote);
	return &remote->fetch;
}

int git_remote_set_pushspec(git_remote *remote, const char *spec)
{
	git_refspec refspec;

	assert(remote && spec);

	if (git_refspec__parse(&refspec, spec, false) < 0)
		return -1;

	git_refspec__free(&remote->push);
	remote->push.src = refspec.src;
	remote->push.dst = refspec.dst;

	return 0;
}

const git_refspec *git_remote_pushspec(git_remote *remote)
{
	assert(remote);
	return &remote->push;
}

const char* git_remote__urlfordirection(git_remote *remote, int direction)
{
	assert(remote);

	if (direction == GIT_DIR_FETCH) {
		return remote->url;
	}

	if (direction == GIT_DIR_PUSH) {
		return remote->pushurl ? remote->pushurl : remote->url;
	}

	return NULL;
}

int git_remote_connect(git_remote *remote, int direction)
{
	git_transport *t;
	const char *url;

	assert(remote);

	url = git_remote__urlfordirection(remote, direction);
	if (url == NULL )
		return -1;

	if (git_transport_new(&t, url) < 0)
		return -1;

	t->progress_cb = remote->callbacks.progress;
	t->cb_data = remote->callbacks.data;

	t->check_cert = remote->check_cert;
	if (t->connect(t, direction) < 0) {
		goto on_error;
	}

	remote->transport = t;

	return 0;

on_error:
	t->free(t);
	return -1;
}

int git_remote_ls(git_remote *remote, git_headlist_cb list_cb, void *payload)
{
	git_vector *refs = &remote->transport->refs;
	unsigned int i;
	git_pkt *p = NULL;

	assert(remote);

	if (!remote->transport || !remote->transport->connected) {
		giterr_set(GITERR_NET, "The remote is not connected");
		return -1;
	}

	git_vector_foreach(refs, i, p) {
		git_pkt_ref *pkt = NULL;

		if (p->type != GIT_PKT_REF)
			continue;

		pkt = (git_pkt_ref *)p;

		if (list_cb(&pkt->head, payload))
			return GIT_EUSER;
	}

	return 0;
}

int git_remote_download(
		git_remote *remote,
		git_transfer_progress_callback progress_cb,
		void *progress_payload)
{
	int error;

	assert(remote);

	if ((error = git_fetch_negotiate(remote)) < 0)
		return error;

	return git_fetch_download_pack(remote, progress_cb, progress_payload);
}

int git_remote_update_tips(git_remote *remote)
{
	int error = 0, autotag;
	unsigned int i = 0;
	git_buf refname = GIT_BUF_INIT;
	git_oid old;
	git_pkt *pkt;
	git_odb *odb;
	git_vector *refs;
	git_remote_head *head;
	git_reference *ref;
	struct git_refspec *spec;
	git_refspec tagspec;

	assert(remote);

	refs = &remote->transport->refs;
	spec = &remote->fetch;

	if (refs->length == 0)
		return 0;

	if (git_repository_odb__weakptr(&odb, remote->repo) < 0)
		return -1;

	if (git_refspec__parse(&tagspec, GIT_REFSPEC_TAGS, true) < 0)
		return -1;

	/* HEAD is only allowed to be the first in the list */
	pkt = refs->contents[0];
	head = &((git_pkt_ref *)pkt)->head;
	if (!strcmp(head->name, GIT_HEAD_FILE)) {
		if (git_reference_create_oid(&ref, remote->repo, GIT_FETCH_HEAD_FILE, &head->oid, 1) < 0)
			return -1;

		i = 1;
		git_reference_free(ref);
	}

	for (; i < refs->length; ++i) {
		git_pkt *pkt = refs->contents[i];
		autotag = 0;

		if (pkt->type == GIT_PKT_REF)
			head = &((git_pkt_ref *)pkt)->head;
		else
			continue;

		/* Ignore malformed ref names (which also saves us from tag^{} */
		if (!git_reference_is_valid_name(head->name))
			continue;

		if (git_refspec_src_matches(spec, head->name)) {
			if (git_refspec_transform_r(&refname, spec, head->name) < 0)
				goto on_error;
		} else if (remote->download_tags != GIT_REMOTE_DOWNLOAD_TAGS_NONE) {

			if (remote->download_tags != GIT_REMOTE_DOWNLOAD_TAGS_ALL)
				autotag = 1;

			if (!git_refspec_src_matches(&tagspec, head->name))
				continue;

			git_buf_clear(&refname);
			if (git_buf_puts(&refname, head->name) < 0)
				goto on_error;
		} else {
			continue;
		}

		if (autotag && !git_odb_exists(odb, &head->oid))
			continue;

		error = git_reference_name_to_oid(&old, remote->repo, refname.ptr);
		if (error < 0 && error != GIT_ENOTFOUND)
			goto on_error;

		if (error == GIT_ENOTFOUND)
			memset(&old, 0, GIT_OID_RAWSZ);

		if (!git_oid_cmp(&old, &head->oid))
			continue;

		/* In autotag mode, don't overwrite any locally-existing tags */
		error = git_reference_create_oid(&ref, remote->repo, refname.ptr, &head->oid, !autotag);
		if (error < 0 && error != GIT_EEXISTS)
			goto on_error;

		git_reference_free(ref);

		if (remote->callbacks.update_tips != NULL) {
			if (remote->callbacks.update_tips(refname.ptr, &old, &head->oid, remote->callbacks.data) < 0)
				goto on_error;
		}
	}

	git_refspec__free(&tagspec);
	git_buf_free(&refname);
	return 0;

on_error:
	git_refspec__free(&tagspec);
	git_buf_free(&refname);
	return -1;

}

int git_remote_connected(git_remote *remote)
{
	assert(remote);
	return remote->transport == NULL ? 0 : remote->transport->connected;
}

void git_remote_stop(git_remote *remote)
{
	git_atomic_set(&remote->transport->cancel, 1);
}

void git_remote_disconnect(git_remote *remote)
{
	assert(remote);

	if (remote->transport != NULL && remote->transport->connected)
			remote->transport->close(remote->transport);
}

void git_remote_free(git_remote *remote)
{
	if (remote == NULL)
		return;

	if (remote->transport != NULL) {
		git_remote_disconnect(remote);

		remote->transport->free(remote->transport);
		remote->transport = NULL;
	}

	git_vector_free(&remote->refs);

	git_refspec__free(&remote->fetch);
	git_refspec__free(&remote->push);
	git__free(remote->url);
	git__free(remote->pushurl);
	git__free(remote->name);
	git__free(remote);
}

struct cb_data {
	git_vector *list;
	regex_t *preg;
};

static int remote_list_cb(const git_config_entry *entry, void *data_)
{
	struct cb_data *data = (struct cb_data *)data_;
	size_t nmatch = 2;
	regmatch_t pmatch[2];
	const char *name = entry->name;

	if (!regexec(data->preg, name, nmatch, pmatch, 0)) {
		char *remote_name = git__strndup(&name[pmatch[1].rm_so], pmatch[1].rm_eo - pmatch[1].rm_so);
		GITERR_CHECK_ALLOC(remote_name);

		if (git_vector_insert(data->list, remote_name) < 0)
			return -1;
	}

	return 0;
}

int git_remote_list(git_strarray *remotes_list, git_repository *repo)
{
	git_config *cfg;
	git_vector list;
	regex_t preg;
	struct cb_data data;
	int error;

	if (git_repository_config__weakptr(&cfg, repo) < 0)
		return -1;

	if (git_vector_init(&list, 4, NULL) < 0)
		return -1;

	if (regcomp(&preg, "^remote\\.(.*)\\.url$", REG_EXTENDED) < 0) {
		giterr_set(GITERR_OS, "Remote catch regex failed to compile");
		return -1;
	}

	data.list = &list;
	data.preg = &preg;
	error = git_config_foreach(cfg, remote_list_cb, &data);
	regfree(&preg);
	if (error < 0) {
		size_t i;
		char *elem;
		git_vector_foreach(&list, i, elem) {
			git__free(elem);
		}

		git_vector_free(&list);

		/* cb error is converted to GIT_EUSER by git_config_foreach */
		if (error == GIT_EUSER)
			error = -1;

		return error;
	}

	remotes_list->strings = (char **)list.contents;
	remotes_list->count = list.length;

	return 0;
}

int git_remote_add(git_remote **out, git_repository *repo, const char *name, const char *url)
{
	git_buf buf = GIT_BUF_INIT;

	if (git_buf_printf(&buf, "+refs/heads/*:refs/remotes/%s/*", name) < 0)
		return -1;

	if (git_remote_new(out, repo, name, url, git_buf_cstr(&buf)) < 0)
		goto on_error;

	git_buf_free(&buf);

	if (git_remote_save(*out) < 0)
		goto on_error;

	return 0;

on_error:
	git_buf_free(&buf);
	git_remote_free(*out);
	return -1;
}

void git_remote_check_cert(git_remote *remote, int check)
{
	assert(remote);

	remote->check_cert = check;
}

void git_remote_set_callbacks(git_remote *remote, git_remote_callbacks *callbacks)
{
	assert(remote && callbacks);

	memcpy(&remote->callbacks, callbacks, sizeof(git_remote_callbacks));

	if (remote->transport) {
		remote->transport->progress_cb = remote->callbacks.progress;
		remote->transport->cb_data = remote->callbacks.data;
	}
}

const git_transfer_progress* git_remote_stats(git_remote *remote)
{
	assert(remote);
	return &remote->stats;
}

int git_remote_autotag(git_remote *remote)
{
	return remote->download_tags;
}

void git_remote_set_autotag(git_remote *remote, int value)
{
	remote->download_tags = value;
}

static int ensure_remote_doesnot_exist(git_repository *repo, const char *name)
{
	int error;
	git_remote *remote;

	error = git_remote_load(&remote, repo, name);

	if (error == GIT_ENOTFOUND)
		return 0;

	if (error < 0)
		return error;

	git_remote_free(remote);

	giterr_set(
		GITERR_CONFIG,
		"Remote '%s' already exists.", name);

	return GIT_EEXISTS;
}

static int rename_remote_config_section(
	git_repository *repo,
	const char *old_name,
	const char *new_name)
{
	git_buf old_section_name = GIT_BUF_INIT,
		new_section_name = GIT_BUF_INIT;
	int error = -1;

	if (git_buf_printf(&old_section_name, "remote.%s", old_name) < 0)
		goto cleanup;

	if (git_buf_printf(&new_section_name, "remote.%s", new_name) < 0)
		goto cleanup;

	error = git_config_rename_section(
		repo,
		git_buf_cstr(&old_section_name),
		git_buf_cstr(&new_section_name));

cleanup:
	git_buf_free(&old_section_name);
	git_buf_free(&new_section_name);

	return error;
}

struct update_data
{
	git_config *config;
	const char *old_remote_name;
	const char *new_remote_name;
};

static int update_config_entries_cb(
	const git_config_entry *entry,
	void *payload)
{
	struct update_data *data = (struct update_data *)payload;

	if (strcmp(entry->value, data->old_remote_name))
		return 0;

	return git_config_set_string(
		data->config,
		entry->name,
		data->new_remote_name);
}

static int update_branch_remote_config_entry(
	git_repository *repo,
	const char *old_name,
	const char *new_name)
{
	git_config *config;
	struct update_data data;

	if (git_repository_config__weakptr(&config, repo) < 0)
		return -1;

	data.config = config;
	data.old_remote_name = old_name;
	data.new_remote_name = new_name;

	return git_config_foreach_match(
		config,
		"branch\\..+\\.remote",
		update_config_entries_cb, &data);
}

static int rename_cb(const char *ref, void *data)
{
	if (git__prefixcmp(ref, GIT_REFS_REMOTES_DIR))
		return 0;
	
	return git_vector_insert((git_vector *)data, git__strdup(ref));
}

static int rename_one_remote_reference(
	git_repository *repo,
	const char *reference_name,
	const char *old_remote_name,
	const char *new_remote_name)
{
	int error = -1;
	git_buf new_name = GIT_BUF_INIT;
	git_reference *reference = NULL;

	if (git_buf_printf(
		&new_name,
		GIT_REFS_REMOTES_DIR "%s%s",
		new_remote_name,
		reference_name + strlen(GIT_REFS_REMOTES_DIR) + strlen(old_remote_name)) < 0)
			return -1;

	if (git_reference_lookup(&reference, repo, reference_name) < 0)
		goto cleanup;

	error = git_reference_rename(reference, git_buf_cstr(&new_name), 0);

cleanup:
	git_reference_free(reference);
	git_buf_free(&new_name);
	return error;
}

static int rename_remote_references(
	git_repository *repo,
	const char *old_name,
	const char *new_name)
{
	git_vector refnames;
	int error = -1;
	unsigned int i;
	char *name;

	if (git_vector_init(&refnames, 8, NULL) < 0)
		goto cleanup;

	if (git_reference_foreach(
		repo,
		GIT_REF_LISTALL,
		rename_cb,
		&refnames) < 0)
			goto cleanup;

	git_vector_foreach(&refnames, i, name) {
		if ((error = rename_one_remote_reference(repo, name, old_name, new_name)) < 0)
			goto cleanup;
	}

	error = 0;
cleanup:
	git_vector_foreach(&refnames, i, name) {
		git__free(name);
	}

	git_vector_free(&refnames);
	return error;
}

static int rename_fetch_refspecs(
	git_remote *remote,
	const char *new_name,
	int (*callback)(const char *problematic_refspec, void *payload),
	void *payload)
{
	git_config *config;
	const git_refspec *fetch_refspec;
	git_buf dst_prefix = GIT_BUF_INIT, serialized = GIT_BUF_INIT;
	const char* pos;
	int error = -1;

	fetch_refspec = git_remote_fetchspec(remote);

	/* Is there a refspec to deal with? */
	if (fetch_refspec->src == NULL &&
		fetch_refspec->dst == NULL)
		return 0;

	if (git_refspec__serialize(&serialized, fetch_refspec) < 0)
		goto cleanup;

	/* Is it an in-memory remote? */
	if (remote->name == '\0') {
		error = (callback(git_buf_cstr(&serialized), payload) < 0) ? GIT_EUSER : 0;
		goto cleanup;
	}

	if (git_buf_printf(&dst_prefix, ":refs/remotes/%s/", remote->name) < 0)
		goto cleanup;

	pos = strstr(git_buf_cstr(&serialized), git_buf_cstr(&dst_prefix));

	/* Does the dst part of the refspec follow the extected standard format? */
	if (!pos) {
		error = (callback(git_buf_cstr(&serialized), payload) < 0) ? GIT_EUSER : 0;
		goto cleanup;
	}

	if (git_buf_splice(
		&serialized,
		pos - git_buf_cstr(&serialized) + strlen(":refs/remotes/"),
		strlen(remote->name), new_name,
		strlen(new_name)) < 0)
			goto cleanup;

	git_refspec__free(&remote->fetch);

	if (git_refspec__parse(&remote->fetch, git_buf_cstr(&serialized), true) < 0)
		goto cleanup;

	if (git_repository_config__weakptr(&config, remote->repo) < 0)
		goto cleanup;

	error = update_config_refspec(config, new_name, &remote->fetch, GIT_DIR_FETCH);

cleanup:
	git_buf_free(&serialized);
	git_buf_free(&dst_prefix);
	return error;
}

int git_remote_rename(
	git_remote *remote,
	const char *new_name,
	int (*callback)(const char *problematic_refspec, void *payload),
	void *payload)
{
	int error;

	assert(remote && new_name);

	if ((error = ensure_remote_doesnot_exist(remote->repo, new_name)) < 0)
		return error;

	if ((error = ensure_remote_name_is_valid(new_name)) < 0)
		return error;

	if (!remote->name) {
		if ((error = rename_fetch_refspecs(
			remote,
			new_name,
			callback,
			payload)) < 0)
			return error;

		remote->name = git__strdup(new_name);

		return git_remote_save(remote);
	}

	if ((error = rename_remote_config_section(
		remote->repo,
		remote->name,
		new_name)) < 0)
			return error;

	if ((error = update_branch_remote_config_entry(
		remote->repo,
		remote->name,
		new_name)) < 0)
			return error;

	if ((error = rename_remote_references(
		remote->repo,
		remote->name,
		new_name)) < 0)
			return error;

	if ((error = rename_fetch_refspecs(
		remote,
		new_name,
		callback,
		payload)) < 0)
		return error;

	git__free(remote->name);
	remote->name = git__strdup(new_name);

	return 0;
}
