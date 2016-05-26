#include <unistd.h>
#include <sys/syscall.h>
#include <search.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>

#include "include/list.h"
#include "include/log.h"

#include "trees.h"

enum kcmp_type {
	KCMP_FILE,
	KCMP_VM,
	KCMP_FILES,
	KCMP_FS,
	KCMP_SIGHAND,
	KCMP_IO,
	KCMP_SYSVSEM,

	KCMP_TYPES,
};

static void *fd_tree_root = NULL;
static void *fd_table_tree_root = NULL;

static int kcmp(int type, pid_t pid1, pid_t pid2, unsigned long idx1, unsigned long idx2)
{
	int ret;

	ret = syscall(SYS_kcmp, pid1, pid2, type, idx1, idx2);

	switch (ret) {
		case 0:
			return 0;
		case 1:
			return -1;
		case 2:
			return 1;
		case -1:
			pr_perror("kcmp (type: %d, pid1: %d, pid2: %d, "
				  "idx1: %ld, idx2: %ld) failed",
				  type, pid1, pid2, idx1, idx2);
			break;
		default:
			pr_perror("kcmp (type: %d, pid1: %d, pid2: %d, "
				  "idx1: %ld, idx2: %ld) returned %d\n",
				  type, pid1, pid2, idx1, idx2);
			break;
	}
	_exit(EXIT_FAILURE);
}

static int compare_fds(const void *a, const void *b)
{
	const struct replace_fd *f = a, *s = b;

	return kcmp(KCMP_FILE, f->pid, s->pid, f->fd, s->fd);
}

int collect_fd(pid_t pid, int fd, struct replace_fd **rfd)
{
	struct replace_fd *new_fd, **found_fd;
	int err = -ENOMEM;

	new_fd = malloc(sizeof(*new_fd));
	if (!new_fd) {
		pr_err("failed to allocate\n");
		return -ENOMEM;
	}
	new_fd->pid = pid;
	new_fd->fd = fd;
	new_fd->shared = false;
	new_fd->file_obj = NULL;

	found_fd = tsearch(new_fd, &fd_tree_root, compare_fds);
	if (!found_fd) {
		pr_err("failed to add new fd object to the tree\n");
		goto free_new_fd;
	}

	if (*found_fd != new_fd)
		(*found_fd)->shared = true;

	*rfd = *found_fd;
	err = 0;

free_new_fd:
	if (*found_fd != new_fd)
		free(new_fd);
	return err;
}

struct fd_table_s {
	pid_t pid;
};

static int compare_fd_tables(const void *a, const void *b)
{
	const struct fd_table_s *f = a, *s = b;

	return kcmp(KCMP_FILES, f->pid, s->pid, 0, 0);
}

int collect_fd_table(pid_t pid, bool *exists)
{
	struct fd_table_s *new_fdt, **found_fdt;
	int err = -ENOMEM;

	new_fdt = malloc(sizeof(*new_fdt));
	if (!new_fdt) {
		pr_err("failed to allocate\n");
		return -ENOMEM;
	}
	new_fdt->pid = pid;

	found_fdt = tsearch(new_fdt, &fd_table_tree_root, compare_fd_tables);
	if (!found_fdt) {
		pr_err("failed to add new fdt object to the tree\n");
		goto free_new_fdt;
	}

	err = 0;

free_new_fdt:
	if (*found_fdt != new_fdt) {
		pr_info("process %d shares fd table with process %d\n", pid,
				(*found_fdt)->pid);
		free(new_fdt);
		*exists = true;
	} else
		*exists = false;
	return err;
}
