/*
 * This merges the file listing in the directory cache index
 * with the actual working directory list, and shows different
 * combinations of the two.
 *
 * Copyright (C) Linus Torvalds, 2005
 */
#include "cache.h"
#include "quote.h"
#include "dir.h"
#include "builtin.h"
#include "tree.h"
#include "parse-options.h"
#include "resolve-undo.h"
#include "string-list.h"

static int abbrev;
static int show_deleted;
static int show_cached;
static int show_others;
static int show_stage;
static int show_unmerged;
static int show_resolve_undo;
static int show_modified;
static int show_killed;
static int show_valid_bit;
static int line_terminator = '\n';
static int debug_mode;

static const char *prefix;
static int max_prefix_len;
static int prefix_len;
static const char **pathspec;
static int error_unmatch;
static char *ps_matched;
static const char *with_tree;
static int exc_given;

static const char *tag_cached = "";
static const char *tag_unmerged = "";
static const char *tag_removed = "";
static const char *tag_other = "";
static const char *tag_killed = "";
static const char *tag_modified = "";
static const char *tag_skip_worktree = "";
static const char *tag_resolve_undo = "";

static void write_name(const char* name, size_t len)
{
	write_name_quoted_relative(name, len, prefix, prefix_len, stdout,
			line_terminator);
}

static void show_dir_entry(const char *tag, struct dir_entry *ent)
{
	int len = max_prefix_len;

	if (len >= ent->len)
		die("git ls-files: internal error - directory entry not superset of prefix");

	if (!match_pathspec(pathspec, ent->name, ent->len, len, ps_matched))
		return;

	fputs(tag, stdout);
	write_name(ent->name, ent->len);
}

static void show_other_files(struct dir_struct *dir)
{
	int i;

	for (i = 0; i < dir->nr; i++) {
		struct dir_entry *ent = dir->entries[i];
		if (!cache_name_is_other(ent->name, ent->len))
			continue;
		show_dir_entry(tag_other, ent);
	}
}

static void show_killed_files(struct dir_struct *dir, struct filter_opts *opts)
{
	int i;
	for (i = 0; i < dir->nr; i++) {
		struct dir_entry *ent = dir->entries[i];
		char *cp, *sp;
		int len, killed = 0;
		struct filter_opts *opts_copy;

		opts_copy = xmalloc(sizeof(*opts));
		memcpy(opts_copy, opts, sizeof(*opts));
		opts_copy->read_staged = 0;
		for (cp = ent->name; cp - ent->name < ent->len; cp = sp + 1) {
			struct cache_entry *ce;

			sp = strchr(cp, '/');
			if (!sp) {
				/* If ent->name is prefix of an entry in the
				 * cache, it will be killed.
				 */
				ce = get_cache_entry_by_name(ent->name, ent->len, opts_copy);
				if (!ce)
					break;
				/* pos points at a name immediately after
				 * ent->name in the cache.  Does it expect
				 * ent->name to be a directory?
				 */
				len = ce_namelen(ce);
				if ((ent->len < len) &&
				    !strncmp(ce->name,
					     ent->name, ent->len) &&
				    ce->name[ent->len] == '/')
					killed = 1;
				break;
			}
			if (0 <= get_cache_entry_pos(ent->name, sp - ent->name, opts_copy)) {
				/* If any of the leading directories in
				 * ent->name is registered in the cache,
				 * ent->name will be killed.
				 */
				killed = 1;
				break;
			}
		}
		if (killed)
			show_dir_entry(tag_killed, dir->entries[i]);
	}
}

static void show_ce_entry(const char *tag, struct cache_entry *ce)
{
	int len = max_prefix_len;

	if (len >= ce_namelen(ce))
		die("git ls-files: internal error - cache entry not superset of prefix");

	if (!match_pathspec(pathspec, ce->name, ce_namelen(ce), len, ps_matched))
		return;

	if (tag && *tag && show_valid_bit &&
	    (ce->ce_flags & CE_VALID)) {
		static char alttag[4];
		memcpy(alttag, tag, 3);
		if (isalpha(tag[0]))
			alttag[0] = tolower(tag[0]);
		else if (tag[0] == '?')
			alttag[0] = '!';
		else {
			alttag[0] = 'v';
			alttag[1] = tag[0];
			alttag[2] = ' ';
			alttag[3] = 0;
		}
		tag = alttag;
	}

	if (!show_stage) {
		fputs(tag, stdout);
	} else {
		printf("%s%06o %s %d\t",
		       tag,
		       ce->ce_mode,
		       find_unique_abbrev(ce->sha1,abbrev),
		       ce_stage(ce));
	}
	write_name(ce->name, ce_namelen(ce));
	if (debug_mode) {
		printf("  ctime: %d:%d\n", ce->ce_ctime.sec, ce->ce_ctime.nsec);
		printf("  mtime: %d:%d\n", ce->ce_mtime.sec, ce->ce_mtime.nsec);
		printf("  dev: %d\tino: %d\n", ce->ce_dev, ce->ce_ino);
		printf("  uid: %d\tgid: %d\n", ce->ce_uid, ce->ce_gid);
		printf("  size: %d\tflags: %x\n", ce->ce_size, ce->ce_flags);
	}
}

static void show_ru_info(void)
{
	struct string_list_item *item;

	if (!the_index.resolve_undo)
		return;

	for_each_string_list_item(item, the_index.resolve_undo) {
		const char *path = item->string;
		struct resolve_undo_info *ui = item->util;
		int i, len;

		len = strlen(path);
		if (len < max_prefix_len)
			continue; /* outside of the prefix */
		if (!match_pathspec(pathspec, path, len, max_prefix_len, ps_matched))
			continue; /* uninterested */
		for (i = 0; i < 3; i++) {
			if (!ui->mode[i])
				continue;
			printf("%s%06o %s %d\t", tag_resolve_undo, ui->mode[i],
			       find_unique_abbrev(ui->sha1[i], abbrev),
			       i + 1);
			write_name(path, len);
		}
	}
}

static int ce_excluded(struct path_exclude_check *check, struct cache_entry *ce)
{
	int dtype = ce_to_dtype(ce);
	return path_excluded(check, ce->name, ce_namelen(ce), &dtype);
}

struct show_opts {
	struct dir_struct *dir;
	struct path_exclude_check *check;
};

static int show_cached_stage(struct cache_entry *ce, void *cb_data)
{
	struct show_opts *show_opts = cb_data;

	if ((show_opts->dir->flags & DIR_SHOW_IGNORED) &&
		!ce_excluded(show_opts->check, ce))
		return 0;
	if (show_unmerged && !ce_stage(ce))
		return 0;
	if (ce->ce_flags & CE_UPDATE)
		return 0;
	show_ce_entry(ce_stage(ce) ? tag_unmerged :
		(ce_skip_worktree(ce) ? tag_skip_worktree : tag_cached), ce);
	return 0;
}

static int show_deleted_modified(struct cache_entry *ce, void *cb_data)
{
	struct stat st;
	int err;
	struct show_opts *show_opts = cb_data;

	if ((show_opts->dir->flags & DIR_SHOW_IGNORED) &&
		!ce_excluded(show_opts->check, ce))
		return 0;
	if (ce->ce_flags & CE_UPDATE)
		return 0;
	if (ce_skip_worktree(ce))
		return 0;
	err = lstat(ce->name, &st);
	if (show_deleted && err)
		show_ce_entry(tag_removed, ce);
	if (show_modified && ce_modified(ce, &st, 0))
		show_ce_entry(tag_modified, ce);
	return 0;
}

static void show_files(struct dir_struct *dir, struct filter_opts *opts)
{
	struct path_exclude_check check;
	struct show_opts show_opts;

	show_opts.dir = dir;
	show_opts.check = &check;

	if ((dir->flags & DIR_SHOW_IGNORED))
		path_exclude_check_init(show_opts.check, show_opts.dir);

	/* For cached/deleted files we don't need to even do the readdir */
	if (show_others || show_killed) {
		fill_directory(show_opts.dir, pathspec);
		if (show_others)
			show_other_files(show_opts.dir);
		if (show_killed)
			show_killed_files(show_opts.dir, opts);
	}
	if (show_cached | show_stage)
		for_each_cache_entry_filtered(opts, show_cached_stage, &show_opts);
	if (show_deleted | show_modified)
		for_each_cache_entry_filtered(opts, show_deleted_modified, &show_opts);

	if ((show_opts.dir->flags & DIR_SHOW_IGNORED))
		path_exclude_check_clear(show_opts.check);
}

static int hoist_unmerged(struct cache_entry *ce, void *cb_data)
{
	if (!ce_stage(ce))
		return 0;
	ce->ce_flags |= CE_STAGEMASK;
	return 0;
}

int mark_entry_to_show(struct cache_entry *ce, void *cb_data)
{
	struct cache_entry **last_stage0 = cb_data;
	switch (ce_stage(ce)) {
	case 0:
		*last_stage0 = ce;
		/* fallthru */
	default:
		return 0;
	case 1:
		/*
		 * If there is stage #0 entry for this, we do not
		 * need to show it.  We use CE_UPDATE bit to mark
		 * such an entry.
		 */
		if (*last_stage0 &&
			!strcmp((*last_stage0)->name, ce->name))
			ce->ce_flags |= CE_UPDATE;
	}
	return 0;
}

/*
 * Read the tree specified with --with-tree option
 * (typically, HEAD) into stage #1 and then
 * squash them down to stage #0.  This is used for
 * --error-unmatch to list and check the path patterns
 * that were given from the command line.  We are not
 * going to write this index out.
 */
void overlay_tree_on_cache(const char *tree_name, const char *prefix, struct filter_opts *opts)
{
	struct tree *tree;
	unsigned char sha1[20];
	struct pathspec pathspec;
	struct cache_entry *last_stage0 = NULL;

	if (get_sha1(tree_name, sha1))
		die("tree-ish %s not found.", tree_name);
	tree = parse_tree_indirect(sha1);
	if (!tree)
		die("bad tree-ish %s", tree_name);

	/* Hoist the unmerged entries up to stage #3 to make room */
	for_each_cache_entry_filtered(opts, hoist_unmerged, NULL);

	if (prefix) {
		static const char *(matchbuf[2]);
		matchbuf[0] = prefix;
		matchbuf[1] = NULL;
		init_pathspec(&pathspec, matchbuf);
		pathspec.items[0].use_wildcard = 0;
	} else
		init_pathspec(&pathspec, NULL);
	if (read_tree(tree, 1, &pathspec))
		die("unable to read tree entries %s", tree_name);

	for_each_cache_entry_filtered(opts, mark_entry_to_show, &last_stage0);
}

int report_path_error(const char *ps_matched, const char **pathspec, const char *prefix)
{
	/*
	 * Make sure all pathspec matched; otherwise it is an error.
	 */
	struct strbuf sb = STRBUF_INIT;
	const char *name;
	int num, errors = 0;
	for (num = 0; pathspec[num]; num++) {
		int other, found_dup;

		if (ps_matched[num])
			continue;
		/*
		 * The caller might have fed identical pathspec
		 * twice.  Do not barf on such a mistake.
		 */
		for (found_dup = other = 0;
		     !found_dup && pathspec[other];
		     other++) {
			if (other == num || !ps_matched[other])
				continue;
			if (!strcmp(pathspec[other], pathspec[num]))
				/*
				 * Ok, we have a match already.
				 */
				found_dup = 1;
		}
		if (found_dup)
			continue;

		name = quote_path_relative(pathspec[num], -1, &sb, prefix);
		error("pathspec '%s' did not match any file(s) known to git.",
		      name);
		errors++;
	}
	strbuf_release(&sb);
	return errors;
}

static const char * const ls_files_usage[] = {
	"git ls-files [options] [<file>...]",
	NULL
};

static int option_parse_z(const struct option *opt,
			  const char *arg, int unset)
{
	line_terminator = unset ? '\n' : '\0';

	return 0;
}

static int option_parse_exclude(const struct option *opt,
				const char *arg, int unset)
{
	struct exclude_list *list = opt->value;

	exc_given = 1;
	add_exclude(arg, "", 0, list);

	return 0;
}

static int option_parse_exclude_from(const struct option *opt,
				     const char *arg, int unset)
{
	struct dir_struct *dir = opt->value;

	exc_given = 1;
	add_excludes_from_file(dir, arg);

	return 0;
}

static int option_parse_exclude_standard(const struct option *opt,
					 const char *arg, int unset)
{
	struct dir_struct *dir = opt->value;

	exc_given = 1;
	setup_standard_excludes(dir);

	return 0;
}

int cmd_ls_files(int argc, const char **argv, const char *cmd_prefix)
{
	int require_work_tree = 0, show_tag = 0;
	char *max_prefix;
	struct dir_struct dir;
	struct filter_opts *opts = xmalloc(sizeof(*opts));
	struct option builtin_ls_files_options[] = {
		{ OPTION_CALLBACK, 'z', NULL, NULL, NULL,
			"paths are separated with NUL character",
			PARSE_OPT_NOARG, option_parse_z },
		OPT_BOOLEAN('t', NULL, &show_tag,
			"identify the file status with tags"),
		OPT_BOOLEAN('v', NULL, &show_valid_bit,
			"use lowercase letters for 'assume unchanged' files"),
		OPT_BOOLEAN('c', "cached", &show_cached,
			"show cached files in the output (default)"),
		OPT_BOOLEAN('d', "deleted", &show_deleted,
			"show deleted files in the output"),
		OPT_BOOLEAN('m', "modified", &show_modified,
			"show modified files in the output"),
		OPT_BOOLEAN('o', "others", &show_others,
			"show other files in the output"),
		OPT_BIT('i', "ignored", &dir.flags,
			"show ignored files in the output",
			DIR_SHOW_IGNORED),
		OPT_BOOLEAN('s', "stage", &show_stage,
			"show staged contents' object name in the output"),
		OPT_BOOLEAN('k', "killed", &show_killed,
			"show files on the filesystem that need to be removed"),
		OPT_BIT(0, "directory", &dir.flags,
			"show 'other' directories' name only",
			DIR_SHOW_OTHER_DIRECTORIES),
		OPT_NEGBIT(0, "empty-directory", &dir.flags,
			"don't show empty directories",
			DIR_HIDE_EMPTY_DIRECTORIES),
		OPT_BOOLEAN('u', "unmerged", &show_unmerged,
			"show unmerged files in the output"),
		OPT_BOOLEAN(0, "resolve-undo", &show_resolve_undo,
			    "show resolve-undo information"),
		{ OPTION_CALLBACK, 'x', "exclude", &dir.exclude_list[EXC_CMDL], "pattern",
			"skip files matching pattern",
			0, option_parse_exclude },
		{ OPTION_CALLBACK, 'X', "exclude-from", &dir, "file",
			"exclude patterns are read from <file>",
			0, option_parse_exclude_from },
		OPT_STRING(0, "exclude-per-directory", &dir.exclude_per_dir, "file",
			"read additional per-directory exclude patterns in <file>"),
		{ OPTION_CALLBACK, 0, "exclude-standard", &dir, NULL,
			"add the standard git exclusions",
			PARSE_OPT_NOARG, option_parse_exclude_standard },
		{ OPTION_SET_INT, 0, "full-name", &prefix_len, NULL,
			"make the output relative to the project top directory",
			PARSE_OPT_NOARG | PARSE_OPT_NONEG, NULL },
		OPT_BOOLEAN(0, "error-unmatch", &error_unmatch,
			"if any <file> is not in the index, treat this as an error"),
		OPT_STRING(0, "with-tree", &with_tree, "tree-ish",
			"pretend that paths removed since <tree-ish> are still present"),
		OPT__ABBREV(&abbrev),
		OPT_BOOLEAN(0, "debug", &debug_mode, "show debugging data"),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(ls_files_usage, builtin_ls_files_options);

	memset(&dir, 0, sizeof(dir));
	prefix = cmd_prefix;
	if (prefix)
		prefix_len = strlen(prefix);
	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, prefix, builtin_ls_files_options,
			ls_files_usage, 0);
	if (show_tag || show_valid_bit) {
		tag_cached = "H ";
		tag_unmerged = "M ";
		tag_removed = "R ";
		tag_modified = "C ";
		tag_other = "? ";
		tag_killed = "K ";
		tag_skip_worktree = "S ";
		tag_resolve_undo = "U ";
	}
	if (show_modified || show_others || show_deleted || (dir.flags & DIR_SHOW_IGNORED) || show_killed)
		require_work_tree = 1;
	if (show_unmerged)
		/*
		 * There's no point in showing unmerged unless
		 * you also show the stage information.
		 */
		show_stage = 1;
	if (dir.exclude_per_dir)
		exc_given = 1;

	if (require_work_tree && !is_inside_work_tree())
		setup_work_tree();

	pathspec = get_pathspec(prefix, argv);

	/* Treat unmatching pathspec elements as errors */
	if (pathspec && error_unmatch) {
		int num;
		for (num = 0; pathspec[num]; num++)
			;
		ps_matched = xcalloc(1, num);
	}

	memset(opts, 0, sizeof(*opts));
	opts->pathspec = pathspec;
	opts->read_staged = 1;
	if (show_resolve_undo)
		opts->read_resolve_undo = 1;
	read_cache_filtered(opts);

	/* Find common prefix for all pathspec's */
	max_prefix = opts->max_prefix;
	max_prefix_len = opts->max_prefix_len;

	if ((dir.flags & DIR_SHOW_IGNORED) && !exc_given)
		die("ls-files --ignored needs some exclude pattern");

	/* With no flags, we default to showing the cached files */
	if (!(show_stage | show_deleted | show_others | show_unmerged |
	      show_killed | show_modified | show_resolve_undo))
		show_cached = 1;

	if (with_tree) {
		/*
		 * Basic sanity check; show-stages and show-unmerged
		 * would not make any sense with this option.
		 */
		if (show_stage || show_unmerged)
			die("ls-files --with-tree is incompatible with -s or -u");
		overlay_tree_on_cache(with_tree, max_prefix, opts);
	}
	show_files(&dir, opts);
	if (show_resolve_undo)
		show_ru_info();

	if (ps_matched) {
		int bad;
		bad = report_path_error(ps_matched, pathspec, prefix);
		if (bad)
			fprintf(stderr, "Did you forget to 'git add'?\n");

		return bad ? 1 : 0;
	}

	return 0;
}
