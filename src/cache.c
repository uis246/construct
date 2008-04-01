/*
 * ircd-ratbox: an advanced Internet Relay Chat Daemon(ircd).
 * cache.c - code for caching files
 *
 * Copyright (C) 2003 Lee Hardy <lee@leeh.co.uk>
 * Copyright (C) 2003-2005 ircd-ratbox development team
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1.Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 2.Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * 3.The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * $Id: cache.c 3436 2007-05-02 19:56:40Z jilles $
 */

#include "stdinc.h"
#include "ircd_defs.h"
#include "common.h"
#include "s_conf.h"
#include "client.h"
#include "hash.h"
#include "cache.h"
#include "sprintf_irc.h"
#include "irc_dictionary.h"
#include "numeric.h"

static BlockHeap *cachefile_heap = NULL;
static BlockHeap *cacheline_heap = NULL;

struct cachefile *user_motd = NULL;
struct cachefile *oper_motd = NULL;
char user_motd_changed[MAX_DATE_STRING];

struct Dictionary *help_dict_oper = NULL;
struct Dictionary *help_dict_user = NULL;

/* init_cache()
 *
 * inputs	-
 * outputs	-
 * side effects - inits the file/line cache blockheaps, loads motds
 */
void
init_cache(void)
{
	cachefile_heap = BlockHeapCreate(sizeof(struct cachefile), CACHEFILE_HEAP_SIZE);
	cacheline_heap = BlockHeapCreate(sizeof(struct cacheline), CACHELINE_HEAP_SIZE);

	user_motd_changed[0] = '\0';

	user_motd = cache_file(MPATH, "ircd.motd", 0);
	oper_motd = cache_file(OPATH, "opers.motd", 0);

	help_dict_oper = irc_dictionary_create(strcasecmp);
	help_dict_user = irc_dictionary_create(strcasecmp);
}

/* cache_file()
 *
 * inputs	- file to cache, files "shortname", flags to set
 * outputs	- pointer to file cached, else NULL
 * side effects -
 */
struct cachefile *
cache_file(const char *filename, const char *shortname, int flags)
{
	FILE *in;
	struct cachefile *cacheptr;
	struct cacheline *lineptr;
	char line[BUFSIZE];
	char *p;

	if((in = fopen(filename, "r")) == NULL)
		return NULL;

	if(strcmp(shortname, "ircd.motd") == 0)
	{
		struct stat sb;
		struct tm *local_tm;

		if(fstat(fileno(in), &sb) < 0)
			return NULL;

		local_tm = localtime(&sb.st_mtime);

		if(local_tm != NULL)
			rb_snprintf(user_motd_changed, sizeof(user_motd_changed),
				 "%d/%d/%d %d:%d",
				 local_tm->tm_mday, local_tm->tm_mon + 1,
				 1900 + local_tm->tm_year, local_tm->tm_hour,
				 local_tm->tm_min);
	}

	cacheptr = BlockHeapAlloc(cachefile_heap);

	strlcpy(cacheptr->name, shortname, sizeof(cacheptr->name));
	cacheptr->flags = flags;

	/* cache the file... */
	while(fgets(line, sizeof(line), in) != NULL)
	{
		if((p = strchr(line, '\n')) != NULL)
			*p = '\0';

		lineptr = BlockHeapAlloc(cacheline_heap);
		if(EmptyString(line))
			strlcpy(lineptr->data, " ", sizeof(lineptr->data));
		else
			strlcpy(lineptr->data, line, sizeof(lineptr->data));
		rb_dlinkAddTail(lineptr, &lineptr->linenode, &cacheptr->contents);
	}

	fclose(in);
	return cacheptr;
}

/* free_cachefile()
 *
 * inputs	- cachefile to free
 * outputs	-
 * side effects - cachefile and its data is free'd
 */
void
free_cachefile(struct cachefile *cacheptr)
{
	rb_dlink_node *ptr;
	rb_dlink_node *next_ptr;

	if(cacheptr == NULL)
		return;

	RB_DLINK_FOREACH_SAFE(ptr, next_ptr, cacheptr->contents.head)
	{
		BlockHeapFree(cacheline_heap, ptr->data);
	}

	BlockHeapFree(cachefile_heap, cacheptr);
}

/* load_help()
 *
 * inputs	-
 * outputs	-
 * side effects - old help cache deleted
 *		- contents of help directories are loaded.
 */
void
load_help(void)
{
	DIR *helpfile_dir = NULL;
	struct dirent *ldirent= NULL;
	char filename[MAXPATHLEN];
	struct cachefile *cacheptr;
	struct DictionaryIter iter;

	DICTIONARY_FOREACH(cacheptr, &iter, help_dict_oper)
	{
		irc_dictionary_delete(help_dict_oper, cacheptr->name);
		free_cachefile(cacheptr);
	}
	DICTIONARY_FOREACH(cacheptr, &iter, help_dict_user)
	{
		irc_dictionary_delete(help_dict_user, cacheptr->name);
		free_cachefile(cacheptr);
	}

	helpfile_dir = opendir(HPATH);

	if(helpfile_dir == NULL)
		return;

	while((ldirent = readdir(helpfile_dir)) != NULL)
	{
		rb_snprintf(filename, sizeof(filename), "%s/%s", HPATH, ldirent->d_name);
		cacheptr = cache_file(filename, ldirent->d_name, HELP_OPER);
		irc_dictionary_add(help_dict_oper, cacheptr->name, cacheptr);
	}

	closedir(helpfile_dir);
	helpfile_dir = opendir(UHPATH);

	if(helpfile_dir == NULL)
		return;

	while((ldirent = readdir(helpfile_dir)) != NULL)
	{
		rb_snprintf(filename, sizeof(filename), "%s/%s", UHPATH, ldirent->d_name);

		cacheptr = cache_file(filename, ldirent->d_name, HELP_USER);
		irc_dictionary_add(help_dict_user, cacheptr->name, cacheptr);
	}

	closedir(helpfile_dir);
}

/* send_user_motd()
 *
 * inputs	- client to send motd to
 * outputs	- client is sent motd if exists, else ERR_NOMOTD
 * side effects -
 */
void
send_user_motd(struct Client *source_p)
{
	struct cacheline *lineptr;
	rb_dlink_node *ptr;
	const char *myname = get_id(&me, source_p);
	const char *nick = get_id(source_p, source_p);

	if(user_motd == NULL || rb_dlink_list_length(&user_motd->contents) == 0)
	{
		sendto_one(source_p, form_str(ERR_NOMOTD), myname, nick);
		return;
	}

	sendto_one(source_p, form_str(RPL_MOTDSTART), myname, nick, me.name);

	RB_DLINK_FOREACH(ptr, user_motd->contents.head)
	{
		lineptr = ptr->data;
		sendto_one(source_p, form_str(RPL_MOTD), myname, nick, lineptr->data);
	}

	sendto_one(source_p, form_str(RPL_ENDOFMOTD), myname, nick);
}

/* send_oper_motd()
 *
 * inputs	- client to send motd to
 * outputs	- client is sent oper motd if exists
 * side effects -
 */
void
send_oper_motd(struct Client *source_p)
{
	struct cacheline *lineptr;
	rb_dlink_node *ptr;

	if(oper_motd == NULL || rb_dlink_list_length(&oper_motd->contents) == 0)
		return;

	sendto_one(source_p, form_str(RPL_OMOTDSTART), 
		   me.name, source_p->name);

	RB_DLINK_FOREACH(ptr, oper_motd->contents.head)
	{
		lineptr = ptr->data;
		sendto_one(source_p, form_str(RPL_OMOTD),
			   me.name, source_p->name, lineptr->data);
	}

	sendto_one(source_p, form_str(RPL_ENDOFOMOTD), 
		   me.name, source_p->name);
}

