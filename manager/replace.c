#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <signal.h>

#include "include/util.h"
#include "include/log.h"
#include "include/socket.h"
#include "include/shm.h"

#include "spfs/context.h"

#include "context.h"
#include "interface.h"
#include "replace.h"
#include "spfs.h"
#include "freeze.h"
#include "swap.h"
#include "swapfd.h"
#include "processes.h"
#include "cgroup.h"

#define ct_run(func, info, ...)							\
({										\
	int _pid, _err, _status;						\
										\
	_pid = fork();								\
	switch (_pid) {								\
		case -1:							\
			pr_perror("failed to fork");				\
			_err = -errno;						\
		case 0:								\
			_err = enter_spfs_context(info);			\
			if (_err)						\
				_exit(-_err);					\
										\
			_exit(func(info, ##__VA_ARGS__));			\
		default:							\
			_err = 0;						\
	}									\
										\
										\
	if (_pid > 0) {								\
		pr_debug("Created child %d in spfs %s context\n",		\
				_pid, info->mnt.id);				\
		_err = collect_child(_pid, &_status, 0);			\
	}									\
	_err ? _err : _status;							\
})

static int spfs_pids_list(struct spfs_info_s *info, char **list)
{
	char *tasks_file;
	int err;

	tasks_file = xsprintf("%s/tasks", info->fg->path);
	if (!tasks_file)
		return -ENOMEM;

	err = get_pids_list(tasks_file, list);

	free(tasks_file);
	return err;
}

static int prepare_mount_env_ct(struct spfs_info_s *info, const char *proxy_dir)
{
	int err;

	err = create_dir("%s", info->work_dir);
	if (err)
		return err;

	if (mount("spfs-manager", info->work_dir, "tmpfs", 0, "size=1m")) {
		pr_err("failed to mount tmpfs to %s", info->work_dir);
		err = -errno;
		goto rm_info_dir;
	}

	if (proxy_dir) {
		err = create_dir(proxy_dir);
		if (err) {
			pr_err("failed to create %s directory\n", proxy_dir);
			goto umount_tmpfs;
		}
	}

	return 0;

umount_tmpfs:
	if (umount(info->work_dir))
		pr_perror("failed to unmount %s", info->work_dir);
rm_info_dir:
	if (rmdir(info->work_dir))
		pr_perror("failed to remove %s", info->work_dir);
	return err;
}

int prepare_mount_env(struct spfs_info_s *info, const char *proxy_dir)
{
	return ct_run(prepare_mount_env_ct, info, proxy_dir);
}

static int cleanup_mount_env_ct(struct spfs_info_s *info)
{
	if (umount2(info->work_dir, MNT_DETACH)) {
		pr_perror("failed to umount %s", info->work_dir);
		return -errno;
	}

	if (rmdir(info->work_dir)) {
		pr_perror("failed to remove directory %s", info->work_dir);
		return -errno;
	}
	return 0;
}

int cleanup_mount_env(struct spfs_info_s *info)
{
	return ct_run(cleanup_mount_env_ct, info);
}

static int do_mount(const char *source, const char *mnt,
		    const char *fstype, unsigned long mountflags,
		    const void *options)
{
	int err;

	err = mount(source, mnt, fstype, mountflags, options);
	if (!err)
		return 0;

	switch (errno) {
		case EPROTONOSUPPORT:
		case EPERM:
			pr_warn("failed to mount %s to %s: %s\n", fstype, mnt,
					strerror(errno));
			return -EAGAIN;
	}
	return -errno;
}

static int mount_loop(struct spfs_info_s *info,
		      const char *source, const char *mnt,
		      const char *fstype, unsigned long mountflags,
		      const void *options)
{
	int err;
	int timeout = 1;

	pr_debug("trying to mount %s, source %s, flags %ld, options '%s' to %s\n",
			fstype, source, mountflags, options, mnt);

	err = create_dir(mnt);
	if (err) {
		pr_err("failed to create mountpoint %s\n", mnt);
		return err;
	}

	while (1) {
		err = do_mount(source, mnt, fstype, mountflags, options);
		if (err != -EAGAIN)
			break;

		pr_warn("retrying in %d seconds\n", timeout);
		sleep(timeout);

		if (timeout < 32)
			timeout <<= 1;
	}

	if (err) {
		pr_perror("failed to mount %s to %s", fstype, mnt);
		goto rmdir_mnt;
	}

	pr_info("Successfully mounted %s to %s\n", fstype, mnt);

	return 0;

rmdir_mnt:
	if (rmdir(mnt))
		pr_perror("failed to remove %s", mnt);
	return err;

}

static int do_replace_one_spfs(const char *source, const char *target)
{
	int err;

	err = umount2(target, MNT_DETACH);
	if (err) {
		pr_perror("failed to umount %s", target);
		return err;
	}

	pr_debug("mountpoint %s was lazily umounted\n", target);

	err = mount(source, target, NULL, MS_BIND, NULL);
	if (err) {
		pr_perror("failed to bind-mount %s to %s", source, target);
		return err;
	}

	pr_debug("mountpoint %s was bind-mounted to %s\n", source, target);
	return 0;
}

static int do_replace_mounts(struct spfs_info_s *info, const char *source)
{
	int err;
	struct spfs_bindmount *bm;
	struct mount_info_s *mnt = &info->mnt;
	int spfs_ref;

	if (stat(mnt->mountpoint, &mnt->st)) {
		pr_perror("failed to stat %s", mnt->mountpoint);
		return -errno;
	}

	spfs_ref = open(mnt->mountpoint, O_RDONLY | O_DIRECTORY);
	if (spfs_ref < 0) {
		pr_perror("failed to open %s", mnt->mountpoint);
		return spfs_ref;
	}

	err = lock_shared_list(&info->mountpaths);
	if (err) {
		pr_err("failed to lock info %s mount paths list\n", mnt->id);
		goto close_spfs_ref;
	}

	list_for_each_entry(bm, &info->mountpaths.list, list) {
		err = do_replace_one_spfs(source, bm->path);
		if (err) {
			pr_err("failed to replace %s by %s\n", bm->path, source);
			goto unlock_shared_list;
		}
	}

	err = spfs_send_mode(info, SPFS_PROXY_MODE, mnt->mountpoint);

unlock_shared_list:
	(void) unlock_shared_list(&info->mountpaths);
close_spfs_ref:
	close(spfs_ref);
	return err;
}

static int spfs_replace_resources(struct spfs_info_s *info, int *ns_fds)
{
	char *pids;
	int err;
	int freezer_state_fd;
	LIST_HEAD(processes);

	freezer_state_fd = open_cgroup_state(info->fg);
	if (freezer_state_fd < 0)
		return freezer_state_fd;

	err = spfs_pids_list(info, &pids);
	if (err)
		return err;

	/* Set target mount and network namespaces to be able to collect opened
	 * files and file mapping information.
	 * Important: we do not change user namespace here, because
	 * /proc/<pid>/map_files won't be accessible.
	 */
	err = set_namespaces(ns_fds, NS_MNT_MASK | NS_NET_MASK);
	if (err)
		goto free_pids;

	err = collect_processes(pids, &processes, &info->mnt);
	if (err)
		goto free_pids;
#if 0
	Looks like user namespace is not required at all?
	err = set_namespaces(ns_fds, NS_USER_MASK);
	if (err)
		goto free_pids;
#endif

	err = write(freezer_state_fd, "THAWED", sizeof("THAWED"));
	if (err != sizeof("THAWED")) {
		pr_perror("Unable to thaw");
		goto free_pids;
	}
	close(freezer_state_fd);

	err = seize_processes(&processes);
	if (err)
		goto free_pids;

	err = do_swap_resources(&processes);
	if (err)
		goto free_pids;

	err = release_processes(&processes);

free_pids:
	free(pids);
	return err;
}

static int do_replace_resources(struct spfs_info_s *info)
{
	int err, status, pid;
	int ct_ns_fds[NS_MAX];

	err = open_namespaces(info->ns_pid, ct_ns_fds);
	if (err) {
		pr_perror("failed to change %d namespaces: %s\n", info->ns_pid,
				info->ns_list);
		return err;
	}

	/* Join target pid namespace to extract virtual pids from freezer cgroup.
	 * This is required, because resources reopen must be performed in
	 * container's context (correct /proc is needed for different checks
	 * and opened file modifications).
	 * Also, ptrace needs to use pids, located in its pid namespace.
	 */
	err = set_namespaces(ct_ns_fds, NS_PID_MASK);
	if (err)
		goto close_namespaces;

	pid = fork();
	switch (pid) {
		case -1:
			pr_perror("failed to fork");
			err = -errno;
		case 0:
			_exit(spfs_replace_resources(info, ct_ns_fds));
	}

	if (pid > 0)
		err = collect_child(pid, &status, 0);

close_namespaces:
	close_namespaces(ct_ns_fds);
	return err ? err : status;
}

static int do_replace_spfs(struct spfs_info_s *info, const char *source)
{
	int err, res;

	if (info->ovz_id) {
		err = move_to_cgroup("ve", "/");
		if (err)
			return err;
	}

	res = spfs_freeze_and_lock(info);
	if (res)
		return res;

	err = ct_run(do_replace_mounts, info, source);
	if (!err)
		err = do_replace_resources(info);

	res = spfs_thaw_and_unlock(info);

	return err ? err : res;
}

static int umount_target(const struct spfs_info_s *info, const char *mnt)
{
	pr_debug("Unmounting %s\n", mnt);

	if (umount2(mnt, MNT_DETACH)) {
		pr_perror("failed to umount %s", mnt);
		return -1;
	}
	return 0;
}

static int do_mount_target(struct spfs_info_s *info,
		const char *source, const char *target, const char *fstype,
		const char *mountflags, const void *options)
{
	int err;
	long mflags;

	err = xatol(mountflags, &mflags);
	if (err)
		return err;

	err = ct_run(mount_loop, info, source, target, fstype, mflags, options);
	if (err)
		return err;

	err = spfs_send_mode(info, SPFS_PROXY_MODE, target);
	if (err)
		/*TODO: should umount the target ? */
		return err;

	return 0;
}

int replace_spfs(int sock, struct spfs_info_s *info,
		  const char *source, const char *fstype,
		  const char *mountflags, const void *options)
{
	char *mnt;
	int err;

	/* TODO dropping inherited handlerforof the SIGCHLD might be done
	 * somehow better.
	 * This is required to prevent a situation, when wait() returns ECHILD
	 * (ct_run()->collect_child)
	 */
	signal(SIGCHLD, SIG_DFL);

	(void) send_status(sock, 0);

	mnt = xsprintf("%s/%s", info->work_dir, fstype);
	if (!mnt) {
		pr_err("failed to allocate\n");
		return -ENOMEM;
	}

	err = do_mount_target(info, source, mnt,
			fstype, mountflags, options);
	if (err)
		goto free_mnt;

	err = do_replace_spfs(info, mnt);
	if (err)
		goto free_mnt;

	(void) ct_run(umount_target, info, mnt);

free_mnt:
	free(mnt);
	return err;
}