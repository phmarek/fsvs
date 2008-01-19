/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include "global.h"
#include "actions.h"
#include "status.h"
#include "commit.h"
#include "update.h"
#include "export.h"
#include "log.h"
#include "ignore.h"
#include "cp_mv.h"
#include "sync.h"
#include "checkout.h"
#include "diff.h"
#include "url.h"
#include "add_unvers.h"
#include "props.h"
#include "info.h"
#include "revert.h"
#include "remote.h"
#include "build.h"


/** \file
 * List of actions, their command line names, and corresponding flags. */


/** Array of command name pointers.
 * The \c acl at the beginning means <i>AC</i>tion <i>L</i>ist. */
static const char 
			*acl_status[]     = { "status", NULL },
			*acl_commit[]     = { "commit", "checkin", "ci", NULL },
			*acl_update[]     = { "update", NULL },
			*acl_export[]     = { "export", NULL },
			*acl_build[]      = { "_build-new-list", NULL },
			*acl_remote[]     = { "remote-status", "rs", NULL },
			*acl_ignore[]     = { "ignore", NULL },
			*acl_add[]        = { "add", NULL },
			*acl_copyfr[]     = { "copyfrom-detect", "copy-detect", NULL },
			*acl_cp[]         = { "copy", "move", "cp", "mv", NULL },
			*acl_unvers[]     = { "unversion", NULL },
			*acl_log[]        = { "log", NULL },
			*acl_checko[]   	= { "checkout", "co", NULL },
			*acl_sync_r[]     = { "sync-repos", NULL },
			*acl_revert[]     = { "revert", "undo", NULL },
			*acl_prop_l[]     = { "prop-list", "pl", NULL },
			*acl_prop_g[]     = { "prop-get", "pg", NULL },
			*acl_prop_s[]     = { "prop-set", "ps", NULL },
			*acl_prop_d[]     = { "prop-del", "pd", NULL },
			*acl_diff[]       = { "diff", NULL },
			*acl_help[]       = { "help", "?", NULL },
			*acl_mergelist[] UNUSED = { "mergelist", NULL },
			*acl_info[]       = { "info", NULL },
			/** \todo: remove initialize */
			*acl_urls[]       = { "urls", "initialize", NULL };


/* A generated file. */
#include "doc.g-c"

/** This \#define is used to save us from writing the member names, in 
 * order to get a nice tabular layout.
 * Simply writing the initializations in structure order is not good;
 * a simple re-arrange could make problems. */
#define ACT(nam, _work, _act, _act_u, ...) \
{ .name=acl_##nam, .help_text=hlp_##nam, \
	.work=_work, .local_callback=_act, .local_uninit=_act_u, \
	__VA_ARGS__ }


/** -. */
struct actionlist_t action_list[]=
{
	/* The first action is the default. */
	ACT(status,   st__work,   st__status,                NULL, .only_opt_filter=1),
	ACT(commit,   ci__work,   ci__action, st__progress_uninit, .only_opt_filter=1),
	ACT(update,   up__work, st__progress, st__progress_uninit, .needs_decoder=1),
	ACT(export,  exp__work,         NULL,                NULL, .is_import_export=1, .needs_decoder=1),
	ACT(unvers,   au__work,   au__action,                NULL, .i_val=RF_UNVERSION),
	ACT(   add,   au__work,   au__action,                NULL, .i_val=RF_ADD),
	ACT(  diff,   df__work,   df__action,                NULL, .needs_decoder=1),
	ACT(sync_r, sync__work,         NULL,                NULL, .repos_feedback=sync__progress),
	ACT(  urls, urls__work,         NULL,                NULL),
	ACT(revert,  rev__work,  rev__action, st__progress_uninit, .needs_decoder=1, .keep_children=1),
	ACT(ignore,  ign__work,         NULL,                NULL),
	ACT(copyfr, cm__detect, st__progress, st__progress_uninit),
	ACT(    cp,   cm__work,         NULL,                NULL),
	ACT(   log,  log__work,         NULL,                NULL),
	ACT(checko,   co__work,         NULL,                NULL, .needs_decoder=1),
	ACT( build,  bld__work,   st__status,                NULL),
	/* For help we set import_export, to avoid needing a WAA 
	 * (default /var/spool/fsvs) to exist. */
	ACT(  help,  ac__Usage,         NULL,                NULL, .is_import_export=1),
	ACT(  info, info__work, info__action,                NULL),
	ACT(prop_g,prp__g_work,         NULL,                NULL),
	ACT(prop_s,prp__s_work,         NULL,                NULL, .i_val=FS_NEW),
	ACT(prop_d,prp__s_work,         NULL,                NULL, .i_val=FS_REMOVED),
	ACT(prop_l,prp__l_work,         NULL,                NULL),
	ACT(remote,   up__work,         NULL,                NULL, .is_compare=1, .repos_feedback=st__rm_status),
};

/** -. */
const int action_list_count = sizeof(action_list)/sizeof(action_list[0]);
/** -. */
struct actionlist_t *action=action_list;

