#include <stdlib.h>

#include "include/util.h"
#include "include/log.h"
#include "include/namespaces.h"

#include "replace.h"
#include "freeze.h"
#include "swap.h"
#include "processes.h"

static int do_replace_resources(struct freeze_cgroup_s *fg,
				const char *source_mnt,
				dev_t src_dev,
				const char *target_mnt,
				int *ns_fds, int *cur_ns_fds)
{
	char *pids;
	int err, ret;
	LIST_HEAD(processes);

	err = cgroup_pids(fg, &pids);
	if (err)
		return err;

	/* We need to set target mount namespace, because we need /proc, where
	 * we can check, whether process being collected is kthread or not.
	 */
	err = set_namespaces(ns_fds, NS_MNT_MASK);
	if (err)
		return err;

	err = collect_processes(pids, &processes);
	if (err)
		goto release_processes;

	/* And we also want to revert mount namespace bask, so we can find the
	 * freezer cgroup to thaw before seize. */
	ret = set_namespaces(cur_ns_fds, NS_MNT_MASK);
	if (ret)
		goto release_processes;

	err = thaw_cgroup(fg);
	if (err)
		goto release_processes;

	/* Set target mount back again, so we can examine processes files.
	 * We do it before seize, becuase of parasite injection, which accesses
	 * process /proc information.
	 */
	err = set_namespaces(ns_fds, NS_MNT_MASK | NS_NET_MASK);
	if (err)
		goto free_pids;

	err = seize_processes(&processes);
	if (err)
		goto release_processes;

	if (source_mnt)
		err = examine_processes_by_mnt(&processes,
					       source_mnt, target_mnt);
	else
		err = examine_processes_by_dev(&processes,
					       src_dev, target_mnt);
	if (err)
		goto release_processes;

	err = do_swap_resources(&processes);

release_processes:
	ret = release_processes(&processes);
free_pids:
	free(pids);
	return err ? err : ret;
}

int __replace_resources(struct freeze_cgroup_s *fg, int *ns_fds,
		      const char *source_mnt, dev_t src_dev,
		      const char *target_mnt,
		      pid_t ns_pid)
{
	int err, status, pid;
	int cur_ns_fds[NS_MAX];

	/* We open current namespaces and pass them to child.
	 * The reason for this is that we are going to fork child in a
	 * different pid namespace, and it's won't be able to find itself by
	 * virtual pid in proc.
	 */
	err = open_namespaces(getpid(), cur_ns_fds);
	if (err)
		return err;

	/* Join target pid namespace to extract virtual pids from freezer cgroup.
	 * This is required, because resources reopen must be performed in
	 * container's context (correct /proc is needed for different checks
	 * and opened file modifications).
	 * Also, ptrace needs to use pids, located in its pid namespace.
	 */
	err = set_namespaces(ns_fds, NS_PID_MASK);
	if (err)
		goto close_cur_ns_fds;

	pid = fork();
	switch (pid) {
		case -1:
			pr_perror("failed to fork");
			err = -errno;
		case 0:
			_exit(do_replace_resources(fg, source_mnt, src_dev,
						   target_mnt,
						   ns_fds, cur_ns_fds));
	}

	if (pid > 0)
		err = collect_child(pid, &status, 0);

close_cur_ns_fds:
	close_namespaces(cur_ns_fds);
	return err ? err : status;
}

int replace_resources(struct freeze_cgroup_s *fg,
		      const char *source_mnt, dev_t src_dev,
		      const char *target_mnt,
		      pid_t ns_pid)
{
	int res, err;
	int ct_ns_fds[NS_MAX], *ns_fds = NULL;

	if (ns_pid) {
		err = open_namespaces(ns_pid, ct_ns_fds);
		if (err) {
			pr_perror("failed to open %d namespaces", ns_pid);
			return err;
		}
		ns_fds = ct_ns_fds;
	}

	res = lock_cgroup(fg);
	if (!res) {
		res = freeze_cgroup(fg);
		if (res)
			(void) unlock_cgroup(fg);
	}
	if (res)
		goto close_ns_fds;

	err = __replace_resources(fg, ns_fds, source_mnt, src_dev, target_mnt, ns_pid);

	res = thaw_cgroup(fg);
	if (!res)
		(void) unlock_cgroup(fg);

close_ns_fds:
	if (ns_fds)
		close_namespaces(ns_fds);
	return err ? err : res;
}
