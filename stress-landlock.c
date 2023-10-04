/*
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-builtin.h"
#include "core-killpid.h"

#if defined(HAVE_LINUX_LANDLOCK_H)
#include <linux/landlock.h>
#endif

#if defined(HAVE_SYS_PRCTL_H)
#include <sys/prctl.h>
#endif

static const stress_help_t help[] = {
	{ NULL,	"landlock N",	  "start N workers stressing landlock file operations" },
	{ NULL,	"landlock-ops N", "stop after N landlock bogo operations" },
	{ NULL,	NULL,		  NULL }
};
#define SHIM_LANDLOCK_CREATE_RULESET_VERSION	(1U << 0)

#define SHIM_LANDLOCK_ACCESS_FS_EXECUTE		(1ULL << 0)
#define SHIM_LANDLOCK_ACCESS_FS_WRITE_FILE	(1ULL << 1)
#define SHIM_LANDLOCK_ACCESS_FS_READ_FILE	(1ULL << 2)
#define SHIM_LANDLOCK_ACCESS_FS_READ_DIR	(1ULL << 3)
#define SHIM_LANDLOCK_ACCESS_FS_REMOVE_DIR	(1ULL << 4)
#define SHIM_LANDLOCK_ACCESS_FS_REMOVE_FILE	(1ULL << 5)
#define SHIM_LANDLOCK_ACCESS_FS_MAKE_CHAR	(1ULL << 6)
#define SHIM_LANDLOCK_ACCESS_FS_MAKE_DIR	(1ULL << 7)
#define SHIM_LANDLOCK_ACCESS_FS_MAKE_REG	(1ULL << 8)
#define SHIM_LANDLOCK_ACCESS_FS_MAKE_SOCK	(1ULL << 9)
#define SHIM_LANDLOCK_ACCESS_FS_MAKE_FIFO	(1ULL << 10)
#define SHIM_LANDLOCK_ACCESS_FS_MAKE_BLOCK	(1ULL << 11)
#define SHIM_LANDLOCK_ACCESS_FS_MAKE_SYM	(1ULL << 12)

#define SHIM_LANDLOCK_ACCESS_ALL		\
	SHIM_LANDLOCK_ACCESS_FS_EXECUTE |	\
	SHIM_LANDLOCK_ACCESS_FS_WRITE_FILE |	\
	SHIM_LANDLOCK_ACCESS_FS_READ_FILE |	\
	SHIM_LANDLOCK_ACCESS_FS_READ_DIR |	\
	SHIM_LANDLOCK_ACCESS_FS_REMOVE_DIR |	\
	SHIM_LANDLOCK_ACCESS_FS_REMOVE_FILE |	\
	SHIM_LANDLOCK_ACCESS_FS_MAKE_CHAR |	\
	SHIM_LANDLOCK_ACCESS_FS_MAKE_DIR |	\
	SHIM_LANDLOCK_ACCESS_FS_MAKE_REG |	\
	SHIM_LANDLOCK_ACCESS_FS_MAKE_SOCK |	\
	SHIM_LANDLOCK_ACCESS_FS_MAKE_FIFO |	\
	SHIM_LANDLOCK_ACCESS_FS_MAKE_BLOCK |	\
	SHIM_LANDLOCK_ACCESS_FS_MAKE_SYM;	\

#if defined(HAVE_LINUX_LANDLOCK_H) &&		\
    defined(HAVE_LANDLOCK_RULE_TYPE) &&		\
    defined(HAVE_LANDLOCK_RULESET_ATTR) &&	\
    defined(HAVE_SYSCALL) &&			\
    defined(__NR_landlock_create_ruleset) &&	\
    defined(__NR_landlock_restrict_self) &&	\
    defined(__NR_landlock_add_rule)

typedef struct {
	uint32_t flag;
	char filename[PATH_MAX];
	const char *path;
} stress_landlock_ctxt_t;

typedef int (*stress_landlock_func)(const stress_args_t *args, stress_landlock_ctxt_t *ctxt);

static int shim_landlock_create_ruleset(
	struct landlock_ruleset_attr *attr,
	size_t size,
	uint32_t flags)
{
#if defined(__NR_landlock_create_ruleset)
	return (int)syscall(__NR_landlock_create_ruleset, attr, size, flags);
#else
	(void)attr;
	(void)size;
	(void)flags;

	errno = ENOSYS;
	return -1;
#endif
}

static int shim_landlock_restrict_self(const int fd, const uint32_t flags)
{
#if defined(__NR_landlock_restrict_self)
	return (int)syscall(__NR_landlock_restrict_self, fd, flags);
#else
	(void)fd;
	(void)flags;

	errno = ENOSYS;
	return -1;
#endif
}

static int shim_landlock_add_rule(
	const int fd,
	const enum landlock_rule_type type,
	const void *const rule_attr,
	const uint32_t flags)
{
#if defined(__NR_landlock_add_rule)
	return (int)syscall(__NR_landlock_add_rule, fd, type, rule_attr, flags);
#else
	(void)fd;
	(void)type;
	(void)rule_attr;
	(void)flags;

	errno = ENOSYS;
	return -1;
#endif
}

static int stress_landlock_supported(const char *name)
{
	int ruleset_fd;
	struct landlock_ruleset_attr ruleset_attr;

	(void)shim_memset(&ruleset_attr, 0, sizeof(ruleset_attr));
	ruleset_attr.handled_access_fs = SHIM_LANDLOCK_ACCESS_FS_READ_FILE;

	ruleset_fd = shim_landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) {
		if (errno == ENOSYS) {
			pr_inf_skip("%s: stressor will be skipped, landlock_create_ruleset system call"
				" is not supported\n", name);
		} else {
			pr_inf_skip("%s: stressor will be skipped, perhaps "
				"lsm=landlock is not enabled\n", name);
		}
		return -1;
	}

	(void)close(ruleset_fd);
	return 0;
}

static int stress_landlock_filter(const struct dirent *d)
{
	return !stress_is_dot_filename(d->d_name);
}

/*
 *  stress_landlock_many()
 *   	recursively apply landlock to as many files as possible to consume
 *   	landlock resources.
 */
static void stress_landlock_many(const stress_args_t *args, const char *path, const int depth)
{
	struct dirent **namelist = NULL;
	int i, n;
	int ruleset_fd, ret;

	struct landlock_ruleset_attr ruleset_attr;

	(void)shim_memset(&ruleset_attr, 0, sizeof(ruleset_attr));
	ruleset_attr.handled_access_fs = SHIM_LANDLOCK_ACCESS_ALL;
	ruleset_fd = shim_landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) {
		pr_inf("%s: landlock_create_ruleset failed, errno=%d (%s), handled_access_fs = 0x%" PRIx64 "\n",
			args->name, errno, strerror(errno), (uint64_t)ruleset_attr.handled_access_fs);
		return;
	}

	n = scandir(path, &namelist, stress_landlock_filter, alphasort);
	for (i = 0; i < n; i++) {
		char newpath[PATH_MAX], resolved[PATH_MAX];

		if (strcmp(path, "/"))
			(void)snprintf(newpath, sizeof(newpath), "%s/%s", path, namelist[i]->d_name);
		else
			(void)snprintf(newpath, sizeof(newpath), "/%s", namelist[i]->d_name);

		if (realpath(newpath, resolved) == NULL)
			goto next;

		if (strcmp(newpath, resolved) == 0) {
			struct landlock_path_beneath_attr path_beneath;

			switch (namelist[i]->d_type) {
			case DT_REG:
			case DT_LNK:
				(void)shim_memset(&path_beneath, 0, sizeof(path_beneath));
				path_beneath.allowed_access = SHIM_LANDLOCK_ACCESS_FS_READ_FILE;
				path_beneath.parent_fd = open(resolved, O_PATH | O_NONBLOCK);
				if (path_beneath.parent_fd < 0)
					goto close_ruleset;
				ret = shim_landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_beneath, 0);
				(void)close(path_beneath.parent_fd);
				if (ret < 0) {
					goto close_ruleset;
				}
				break;
			case DT_DIR:
				if (depth < 30)
					stress_landlock_many(args, resolved, depth + 1);
				break;
			default:
				break;
			}
		}
next:
		free(namelist[i]);
	}
	if (namelist)
		free(namelist);

close_ruleset:
	(void)close(ruleset_fd);
}

static int stress_landlock_flag(const stress_args_t *args, stress_landlock_ctxt_t *ctxt)
{
	int ruleset_fd, fd, ret, rc = EXIT_SUCCESS;
	struct landlock_ruleset_attr ruleset_attr;
	struct landlock_path_beneath_attr path_beneath;

	/* Create empty test file */
	fd = open(ctxt->filename, O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
	if (fd > -1)
		(void)close(fd);

	/* Exercise fetch of ruleset API version, ignore return */
	VOID_RET(int, shim_landlock_create_ruleset(NULL, 0, SHIM_LANDLOCK_CREATE_RULESET_VERSION));

	(void)shim_memset(&ruleset_attr, 0, sizeof(ruleset_attr));
	ruleset_attr.handled_access_fs = SHIM_LANDLOCK_ACCESS_ALL;
	ruleset_fd = shim_landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) {
		pr_inf("%s: landlock_create_ruleset failed, errno=%d (%s), handled_access_fs = 0x%" PRIx64 "\n",
			args->name, errno, strerror(errno), (uint64_t)ruleset_attr.handled_access_fs);
		return 0;
	}

	(void)shim_memset(&path_beneath, 0, sizeof(path_beneath));
	path_beneath.allowed_access = ctxt->flag;
	path_beneath.parent_fd = open(ctxt->path, O_PATH);
	if (path_beneath.parent_fd < 0)
		goto close_ruleset;
	ret = shim_landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_beneath, 0);
	if (ret < 0)
		goto close_parent;

	ret = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (ret < 0)
		goto close_parent;

	ret = shim_landlock_restrict_self(ruleset_fd, 0);
	if (ret < 0) {
		pr_inf("%s: landlock_restrict_self failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		goto close_parent;
	}

	/*
	 *  Got a valid landlocked restricted child process,
	 *  so now sanity check it on some test files
	 */
	fd = open(ctxt->filename, O_RDONLY);
	if (fd > -1)
		(void)close(fd);

	fd = open(ctxt->filename, O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd > -1)
		(void)close(fd);

	fd = open(ctxt->filename, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd > -1)
		(void)close(fd);
	(void)shim_unlink(ctxt->filename);

close_parent:
	(void)close(path_beneath.parent_fd);
close_ruleset:
	(void)close(ruleset_fd);

	return rc;
}

static void stress_landlock_test(
	const stress_args_t *args,
	stress_landlock_func func,
	stress_landlock_ctxt_t *ctxt,
	int *failures)
{
	int status;
	pid_t pid;

again:
	pid = fork();
	if (pid < 0) {
		if (stress_redo_fork(args, errno))
			goto again;
		return;
	} else if (pid == 0) {
		_exit(func(args, ctxt));
	} else {
		if (shim_waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR) {
				pr_err("%s: waitpid errno=%d (%s)\n",
					args->name, errno, strerror(errno));
			} else {
				/* Probably an SIGARLM, force kill & reap */
				(void)stress_kill_pid_wait(pid, NULL);
				(void)shim_unlink(ctxt->filename);
				return;
			}
		}
		if (WIFEXITED(status)) {
			int rc = WEXITSTATUS(status);

			if (rc != EXIT_SUCCESS)
				(*failures)++;
			(void)shim_unlink(ctxt->filename);
			return;
		}
		(void)shim_unlink(ctxt->filename);
	}
}

/*
 *  stress_landlock()
 *	stress landlock API
 */
static int stress_landlock(const stress_args_t *args)
{
	static const int landlock_access_flags[] = {
		SHIM_LANDLOCK_ACCESS_FS_EXECUTE,
		SHIM_LANDLOCK_ACCESS_FS_WRITE_FILE,
		SHIM_LANDLOCK_ACCESS_FS_READ_FILE,
		SHIM_LANDLOCK_ACCESS_FS_WRITE_FILE | SHIM_LANDLOCK_ACCESS_FS_READ_FILE,
		SHIM_LANDLOCK_ACCESS_FS_READ_DIR,
		SHIM_LANDLOCK_ACCESS_FS_REMOVE_DIR,
		SHIM_LANDLOCK_ACCESS_FS_REMOVE_FILE,
		SHIM_LANDLOCK_ACCESS_FS_MAKE_CHAR,
		SHIM_LANDLOCK_ACCESS_FS_MAKE_DIR,
		SHIM_LANDLOCK_ACCESS_FS_MAKE_REG,
		SHIM_LANDLOCK_ACCESS_FS_MAKE_SOCK,
		SHIM_LANDLOCK_ACCESS_FS_MAKE_FIFO,
		SHIM_LANDLOCK_ACCESS_FS_MAKE_BLOCK,
		SHIM_LANDLOCK_ACCESS_FS_MAKE_SYM,
		0,
	};
	stress_landlock_ctxt_t ctxt;
	int failures = 0;
	pid_t pid_many;

	ctxt.path = stress_get_temp_path();
	(void)snprintf(ctxt.filename, sizeof(ctxt.filename), "%s/landlock-%" PRIdMAX,
			ctxt.path, (intmax_t)getpid());

again:
	pid_many = fork();
	if (pid_many < 0) {
		if (stress_redo_fork(args, errno))
			goto again;
	} else if (pid_many == 0) {
		do {
			stress_landlock_many(args, "/", 0);
		} while (stress_continue(args));
		_exit(0);
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		size_t i;

		ctxt.flag = 0;

		/* Exercise with a mix of valid and invalid flags */
		for (i = 0; i < SIZEOF_ARRAY(landlock_access_flags); i++) {
			ctxt.flag |= (uint32_t)landlock_access_flags[i];
			stress_landlock_test(args, stress_landlock_flag, &ctxt, &failures);
			if (failures >= 5)
				goto err;
		}
		for (i = 0; i < SIZEOF_ARRAY(landlock_access_flags); i++) {
			ctxt.flag = (uint32_t)landlock_access_flags[i];
			stress_landlock_test(args, stress_landlock_flag, &ctxt, &failures);
			if (failures >= 5)
				goto err;
		}
		ctxt.flag = ~ctxt.flag;
		stress_landlock_test(args, stress_landlock_flag, &ctxt, &failures);
		if (failures >= 5)
			goto err;

		stress_bogo_inc(args);
	} while (stress_continue(args));

err:
	if (pid_many != -1)
		(void)stress_kill_pid_wait(pid_many, NULL);

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	return EXIT_SUCCESS;
}

stressor_info_t stress_landlock_info = {
	.stressor = stress_landlock,
	.class = CLASS_OS,
	.supported = stress_landlock_supported,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_landlock_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help,
	.unimplemented_reason = "linux/landlock.h or __NR_landlock* syscall macros"
};
#endif
