#ifndef __SPFS_MANAGER_MOUNT_H_
#define __SPFS_MANAGER_MOUNT_H_

#include <sys/stat.h>

#include "include/list.h"

struct mount_info_s {
	struct list_head	list;
	char			*id;
	char			*mountpoint;
	char			*ns_mountpoint;
	struct stat		st;
};

struct mount_info_s *iterate_mounts(struct shared_list *mounts, const void *data,
				    bool (*actor)(const struct mount_info_s *info, const void *data));

struct mount_info_s *find_mount_by_id(struct shared_list *mounts, const char *id);
int add_mount_info(struct shared_list *mounts, struct mount_info_s *info);
void del_mount_info(struct shared_list *mounts, struct mount_info_s *info);

int init_mount_info(struct mount_info_s *mnt, const char *id,
		    const char *mountpoint, const char *ns_mountpoint);

int mount_loop(const char *source, const char *mnt,
	       const char *fstype, unsigned long mountflags,
	       const void *options);

#endif
