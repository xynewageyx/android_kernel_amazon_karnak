 * fs/sdcardfs/packagelist.c
 *
 * Copyright (c) 2013 Samsung Electronics Co. Ltd
 *   Authors: Daeho Jeong, Woojoong Lee, Seunghwan Hyun,
 *               Sunghwan Yun, Sungjong Seo
 *
 * This program has been developed as a stackable file system based on
 * the WrapFS which written by
 *
 * Copyright (c) 1998-2011 Erez Zadok
 * Copyright (c) 2009     Shrikar Archak
 * Copyright (c) 2003-2011 Stony Brook University
 * Copyright (c) 2003-2011 The Research Foundation of SUNY
 *
 * This file is dual licensed.  It may be redistributed and/or modified
 * under the terms of the Apache 2.0 License OR version 2 of the GNU
 * General Public License.
 */

#include "sdcardfs.h"
#include <linux/hashtable.h>
#include <linux/delay.h>
#include <linux/radix-tree.h>


#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <linux/configfs.h>

struct hashtable_entry {
	struct hlist_node hlist;
	const char *key;
	atomic_t value;
};

static DEFINE_HASHTABLE(package_to_appid, 8);
static DEFINE_HASHTABLE(package_to_userid, 8);
static DEFINE_HASHTABLE(ext_to_groupid, 8);


static struct kmem_cache *hashtable_entry_cachep;

static unsigned int str_hash(const char *key) {
	int i;
	unsigned int h = strlen(key);
	char *data = (char *)key;

	for (i = 0; i < strlen(key); i++) {
		h = h * 31 + *data;
		data++;
	}
	return h;
}

appid_t get_appid(const char *key)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = str_hash(key);
	appid_t ret_id;

	rcu_read_lock();
	hash_for_each_possible_rcu(package_to_appid, hash_cur, hlist, hash) {
		if (!strcasecmp(key, hash_cur->key)) {
			ret_id = atomic_read(&hash_cur->value);
			rcu_read_unlock();
			return ret_id;
		}
	}
	rcu_read_unlock();
	return 0;
}

appid_t get_ext_gid(const char *key)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = str_hash(key);
	appid_t ret_id;

	rcu_read_lock();
	hash_for_each_possible_rcu(ext_to_groupid, hash_cur, hlist, hash) {
		if (!strcasecmp(key, hash_cur->key)) {
			ret_id = atomic_read(&hash_cur->value);
			rcu_read_unlock();
			return ret_id;
		}
	}
	rcu_read_unlock();
	return 0;
}

/* Kernel has already enforced everything we returned through
 * derive_permissions_locked(), so this is used to lock down access
 * even further, such as enforcing that apps hold sdcard_rw. */
int check_caller_access_to_name(struct inode *parent_node, const char* name) {

	/* Always block security-sensitive files at root */
	if (parent_node && SDCARDFS_I(parent_node)->perm == PERM_ROOT) {
		if (!strcasecmp(name, "autorun.inf")
			|| !strcasecmp(name, ".android_secure")
			|| !strcasecmp(name, "android_secure")) {
			return 0;
		}
	}

	/* Root always has access; access for any other UIDs should always
	 * be controlled through packages.list. */
	if (from_kuid(&init_user_ns, current_fsuid()) == 0) {
		return 1;
	}

	/* No extra permissions to enforce */
	return 1;
}

/* This function is used when file opening. The open flags must be
 * checked before calling check_caller_access_to_name() */
int open_flags_to_access_mode(int open_flags) {
	if((open_flags & O_ACCMODE) == O_RDONLY) {
		return 0; /* R_OK */
	} else if ((open_flags & O_ACCMODE) == O_WRONLY) {
		return 1; /* W_OK */
	} else {
		/* Probably O_RDRW, but treat as default to be safe */
		return 1; /* R_OK | W_OK */
	}
}

static struct hashtable_entry *alloc_hashtable_entry(const char *key,
		appid_t value)
{
	struct hashtable_entry *ret = kmem_cache_alloc(hashtable_entry_cachep,
			GFP_KERNEL);
	if (!ret)
		return NULL;

	ret->key = kstrdup(key, GFP_KERNEL);
	if (!ret->key) {
		kmem_cache_free(hashtable_entry_cachep, ret);
		return NULL;
	}

	atomic_set(&ret->value, value);
	return ret;
}

static int insert_packagelist_entry_locked(const char *key, appid_t value)
{
	struct hashtable_entry *hash_cur;
	struct hashtable_entry *new_entry;
	unsigned int hash = str_hash(key);

	hash_for_each_possible_rcu(package_to_appid, hash_cur, hlist, hash) {
		if (!strcasecmp(key, hash_cur->key)) {
			atomic_set(&hash_cur->value, value);
			return 0;
		}
	}
	new_entry = alloc_hashtable_entry(key, value);
	if (!new_entry)
		return -ENOMEM;
	hash_add_rcu(package_to_appid, &new_entry->hlist, hash);
	return 0;
}

static int insert_ext_gid_entry_locked(const char *key, appid_t value)
{
	struct hashtable_entry *hash_cur;
	struct hashtable_entry *new_entry;
	unsigned int hash = str_hash(key);

	/* An extension can only belong to one gid */
	hash_for_each_possible_rcu(ext_to_groupid, hash_cur, hlist, hash) {
		if (!strcasecmp(key, hash_cur->key))
			return -EINVAL;
	}
	new_entry = alloc_hashtable_entry(key, value);
	if (!new_entry)
		return -ENOMEM;
	hash_add_rcu(ext_to_groupid, &new_entry->hlist, hash);
	return 0;
}

static int insert_userid_exclude_entry_locked(const char *key, userid_t value)
{
	struct hashtable_entry *hash_cur;
	struct hashtable_entry *new_entry;
	unsigned int hash = str_hash(key);

	/* Only insert if not already present */
	hash_for_each_possible_rcu(package_to_userid, hash_cur, hlist, hash) {
		if (atomic_read(&hash_cur->value) == value && !strcasecmp(key, hash_cur->key))
			return 0;
	}
	new_entry = alloc_hashtable_entry(key, value);
	if (!new_entry)
		return -ENOMEM;
	hash_add_rcu(package_to_userid, &new_entry->hlist, hash);
	return 0;
}

static void fixup_all_perms_name(const char *key)
{
	struct sdcardfs_sb_info *sbinfo;
	struct limit_search limit = {
		.flags = BY_NAME,
		.name = key,
		.length = strlen(key),
	};
	list_for_each_entry(sbinfo, &sdcardfs_super_list, list) {
		if (sbinfo_has_sdcard_magic(sbinfo))
			fixup_perms_recursive(sbinfo->sb->s_root, &limit);
	}
}

static void fixup_all_perms(const char *key)
{
	struct sdcardfs_sb_info *sbinfo;
	list_for_each_entry(sbinfo, &sdcardfs_super_list, list)
		if (sbinfo)
			fixup_perms(sbinfo->sb, key);
}

static int insert_packagelist_entry(const char *key, appid_t value)
{
	int err;

	mutex_lock(&sdcardfs_super_list_lock);
	err = insert_packagelist_entry_locked(key, value);
	if (!err)
		fixup_all_perms_name(key);
	mutex_unlock(&sdcardfs_super_list_lock);

	return err;
}

static int insert_ext_gid_entry(const char *key, appid_t value)
{
	int err;

	mutex_lock(&sdcardfs_super_list_lock);
	err = insert_ext_gid_entry_locked(key, value);
	mutex_unlock(&sdcardfs_super_list_lock);

	return err;
}

static int insert_userid_exclude_entry(const char *key, userid_t value)
{
	int err;

	mutex_lock(&sdcardfs_super_list_lock);
	err = insert_userid_exclude_entry_locked(key, value);
	if (!err)
		fixup_all_perms_name_userid(key, value);
	mutex_unlock(&sdcardfs_super_list_lock);

	return err;
}

static void free_hashtable_entry(struct hashtable_entry *entry)
{
	kfree(entry->key);
	hash_del_rcu(&entry->hlist);
	kmem_cache_free(hashtable_entry_cachep, entry);
}

static void remove_packagelist_entry_locked(const char *key)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = str_hash(key);

	hash_for_each_possible_rcu(package_to_appid, hash_cur, hlist, hash) {
		if (!strcasecmp(key, hash_cur->key)) {
			hash_del_rcu(&hash_cur->hlist);
			hlist_add_head(&hash_cur->dlist, &free_list);
			break;
		}
	}
	synchronize_rcu();
	hlist_for_each_entry_safe(hash_cur, h_t, &free_list, dlist)
		free_hashtable_entry(hash_cur);
}

static void remove_packagelist_entry(const char *key)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_packagelist_entry_locked(key);
	fixup_all_perms_name(key);
	mutex_unlock(&sdcardfs_super_list_lock);
	return;
}

static void remove_ext_gid_entry_locked(const char *key, gid_t group)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = str_hash(key);

	hash_for_each_possible_rcu(ext_to_groupid, hash_cur, hlist, hash) {
		if (!strcasecmp(key, hash_cur->key) && atomic_read(&hash_cur->value) == group) {
			hash_del_rcu(&hash_cur->hlist);
			synchronize_rcu();
			free_hashtable_entry(hash_cur);
			break;
		}
	}
}

static void remove_ext_gid_entry(const char *key, gid_t group)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_ext_gid_entry_locked(key, group);
	mutex_unlock(&sdcardfs_super_list_lock);
	return;
}

static void remove_userid_all_entry_locked(userid_t userid)
{
	struct hashtable_entry *hash_cur;
	struct hlist_node *h_t;
	HLIST_HEAD(free_list);
	int i;

	hash_for_each_rcu(package_to_userid, i, hash_cur, hlist) {
		if (atomic_read(&hash_cur->value) == userid) {
			hash_del_rcu(&hash_cur->hlist);
			hlist_add_head(&hash_cur->dlist, &free_list);
		}
	}
	synchronize_rcu();
	hlist_for_each_entry_safe(hash_cur, h_t, &free_list, dlist) {
		free_hashtable_entry(hash_cur);
	}
}

static void remove_userid_all_entry(userid_t userid)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_userid_all_entry_locked(userid);
	fixup_all_perms_userid(userid);
	mutex_unlock(&sdcardfs_super_list_lock);
	return;
}

static void remove_userid_exclude_entry_locked(const char *key, userid_t userid)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = str_hash(key);

	hash_for_each_possible_rcu(package_to_userid, hash_cur, hlist, hash) {
		if (!strcasecmp(key, hash_cur->key) && atomic_read(&hash_cur->value) == userid) {
			hash_del_rcu(&hash_cur->hlist);
			synchronize_rcu();
			free_hashtable_entry(hash_cur);
			break;
		}
	}
}

static void remove_packagelist_entry(const char *key)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_packagelist_entry_locked(key);
	fixup_all_perms(key);
	mutex_unlock(&sdcardfs_super_list_lock);
	return;
}

static void packagelist_destroy(void)
{
	struct hashtable_entry *hash_cur;
	struct hlist_node *h_t;
	HLIST_HEAD(free_list);
	int i;
	mutex_lock(&sdcardfs_super_list_lock);
	hash_for_each_rcu(package_to_appid, i, hash_cur, hlist) {
		hash_del_rcu(&hash_cur->hlist);
		hlist_add_head(&hash_cur->hlist, &free_list);

	}
	synchronize_rcu();
	hlist_for_each_entry_safe(hash_cur, h_t, &free_list, dlist)
		free_hashtable_entry(hash_cur);
	mutex_unlock(&sdcardfs_super_list_lock);
	printk(KERN_INFO "sdcardfs: destroyed packagelist pkgld\n");
}

struct package_details {
	struct config_item item;
	const char* name;
};

static inline struct package_details *to_package_details(struct config_item *item)
{
	return item ? container_of(item, struct package_details, item) : NULL;
}

CONFIGFS_ATTR_STRUCT(package_details);
#define PACKAGE_DETAILS_ATTR(_name, _mode, _show, _store)	\
struct package_details_attribute package_details_attr_##_name = __CONFIGFS_ATTR(_name, _mode, _show, _store)
#define PACKAGE_DETAILS_ATTRIBUTE(name) &package_details_attr_##name.attr

static ssize_t package_details_appid_show(struct package_details *package_details,
				      char *page)
{
	return scnprintf(page, PAGE_SIZE, "%u\n", get_appid(package_details->name));
}

static ssize_t package_details_appid_store(struct package_details *package_details,
				       const char *page, size_t count)
{
	unsigned int tmp;
	int ret;

	ret = kstrtouint(page, 10, &tmp);
	if (ret)
		return ret;

	ret = insert_packagelist_entry(package_details->name, tmp);

	if (ret)
		return ret;

	return count;
}

static void package_details_release(struct config_item *item)
{
	struct package_details *package_details = to_package_details(item);
	printk(KERN_INFO "sdcardfs: removing %s\n", package_details->name);
	remove_packagelist_entry(package_details->name);
	kfree(package_details->name);
	kfree(package_details);
}

PACKAGE_DETAILS_ATTR(appid, S_IRUGO | S_IWUGO, package_details_appid_show, package_details_appid_store);

static struct configfs_attribute *package_details_attrs[] = {
	PACKAGE_DETAILS_ATTRIBUTE(appid),
	PACKAGE_DETAILS_ATTRIBUTE(excluded_userids),
	PACKAGE_DETAILS_ATTRIBUTE(clear_userid),
	NULL,
};

CONFIGFS_ATTR_OPS(package_details);
static struct configfs_item_operations package_details_item_ops = {
	.release = package_details_release,
	.show_attribute = package_details_attr_show,
	.store_attribute = package_details_attr_store,
};

static struct config_item_type package_appid_type = {
	.ct_item_ops	= &package_details_item_ops,
	.ct_attrs	= package_details_attrs,
	.ct_owner	= THIS_MODULE,
};

struct extensions_value {
	struct config_group group;
	unsigned int num;
};

struct extension_details {
	struct config_item item;
	const char* name;
	unsigned int num;
};

static inline struct extensions_value *to_extensions_value(struct config_item *item)
{
	return item ? container_of(to_config_group(item), struct extensions_value, group) : NULL;
}

static inline struct extension_details *to_extension_details(struct config_item *item)
{
	return item ? container_of(item, struct extension_details, item) : NULL;
}

static void extension_details_release(struct config_item *item)
{
	struct extension_details *extension_details = to_extension_details(item);

	printk(KERN_INFO "sdcardfs: No longer mapping %s files to gid %d\n",
			extension_details->name, extension_details->num);
	remove_ext_gid_entry(extension_details->name, extension_details->num);
	kfree(extension_details->name);
	kfree(extension_details);
}

static struct configfs_item_operations extension_details_item_ops = {
	.release = extension_details_release,
};

static struct config_item_type extension_details_type = {
	.ct_item_ops = &extension_details_item_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_item *extension_details_make_item(struct config_group *group, const char *name)
{
	struct extensions_value *extensions_value = to_extensions_value(&group->cg_item);
	struct extension_details *extension_details = kzalloc(sizeof(struct extension_details), GFP_KERNEL);
	int ret;
	if (!extension_details)
		return ERR_PTR(-ENOMEM);

	extension_details->name = kstrdup(name, GFP_KERNEL);
	if (!extension_details->name) {
		kfree(extension_details);
		return ERR_PTR(-ENOMEM);
	}
	extension_details->num = extensions_value->num;
	ret = insert_ext_gid_entry(name, extensions_value->num);

	if (ret) {
		kfree(extension_details->name);
		kfree(extension_details);
		return ERR_PTR(ret);
	}
	config_item_init_type_name(&extension_details->item, name, &extension_details_type);

	return &extension_details->item;
}

static struct configfs_group_operations extensions_value_group_ops = {
	.make_item = extension_details_make_item,
};

static struct config_item_type extensions_name_type = {
	.ct_group_ops	= &extensions_value_group_ops,
	.ct_owner	= THIS_MODULE,
};

static struct config_group *extensions_make_group(struct config_group *group, const char *name)
{
	struct extensions_value *extensions_value;
	unsigned int tmp;
	int ret;

	extensions_value = kzalloc(sizeof(struct extensions_value), GFP_KERNEL);
	if (!extensions_value)
		return ERR_PTR(-ENOMEM);
	ret = kstrtouint(name, 10, &tmp);
	if (ret) {
		kfree(extensions_value);
		return ERR_PTR(ret);
	}

	extensions_value->num = tmp;
	config_group_init_type_name(&extensions_value->group, name,
						&extensions_name_type);
	return &extensions_value->group;
}

static void extensions_drop_group(struct config_group *group, struct config_item *item)
{
	struct extensions_value *value = to_extensions_value(item);
	printk(KERN_INFO "sdcardfs: No longer mapping any files to gid %d\n", value->num);
	kfree(value);
}

static struct configfs_group_operations extensions_group_ops = {
	.make_group	= extensions_make_group,
	.drop_item	= extensions_drop_group,
};

static struct config_item_type extensions_type = {
	.ct_group_ops	= &extensions_group_ops,
	.ct_owner	= THIS_MODULE,
};

struct config_group extension_group = {
	.cg_item = {
		.ci_namebuf = "extensions",
		.ci_type = &extensions_type,
	},
};

struct packages {
	struct configfs_subsystem subsystem;
};

static inline struct packages *to_packages(struct config_item *item)
{
	return item ? container_of(to_configfs_subsystem(to_config_group(item)), struct packages, subsystem) : NULL;
}

CONFIGFS_ATTR_STRUCT(packages);
#define PACKAGES_ATTR(_name, _mode, _show, _store)	\
struct packages_attribute packages_attr_##_name = __CONFIGFS_ATTR(_name, _mode, _show, _store)
#define PACKAGES_ATTR_RO(_name, _show)	\
struct packages_attribute packages_attr_##_name = __CONFIGFS_ATTR_RO(_name, _show);

static struct config_item *packages_make_item(struct config_group *group, const char *name)
{
	struct package_details *package_details;

	package_details = kzalloc(sizeof(struct package_details), GFP_KERNEL);
	if (!package_details)
		return ERR_PTR(-ENOMEM);
	package_details->name = kstrdup(name, GFP_KERNEL);
	if (!package_details->name)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(&package_details->item, name,
				   &package_appid_type);

	return &package_details->item;
}

static ssize_t packages_list_show(struct packages *packages,
					 char *page)
{
	struct hashtable_entry *hash_cur;
	int i;
	int count = 0, written = 0;
	const char errormsg[] = "<truncated>\n";

	rcu_read_lock();
	hash_for_each_rcu(package_to_appid, i, hash_cur, hlist) {
		written = scnprintf(page + count, PAGE_SIZE - sizeof(errormsg) - count, "%s %d\n",
					(const char *)hash_cur->key, atomic_read(&hash_cur->value));
		if (count + written == PAGE_SIZE - sizeof(errormsg)) {
			count += scnprintf(page + count, PAGE_SIZE - count, errormsg);
			break;
		}
		count += written;
	}
	rcu_read_unlock();

	return count;
}

struct packages_attribute packages_attr_packages_gid_list = __CONFIGFS_ATTR_RO(packages_gid.list, packages_list_show);
PACKAGES_ATTR(remove_userid, S_IWUGO, NULL, packages_remove_userid_store);

static struct configfs_attribute *packages_attrs[] = {
	&packages_attr_packages_gid_list.attr,
	NULL,
};

CONFIGFS_ATTR_OPS(packages)
static struct configfs_item_operations packages_item_ops = {
	.show_attribute = packages_attr_show,
	.store_attribute = packages_attr_store,
};

/*
 * Note that, since no extra work is required on ->drop_item(),
 * no ->drop_item() is provided.
 */
static struct configfs_group_operations packages_group_ops = {
	.make_item	= packages_make_item,
};

static struct config_item_type packages_type = {
	.ct_item_ops	= &packages_item_ops,
	.ct_group_ops	= &packages_group_ops,
	.ct_attrs	= packages_attrs,
	.ct_owner	= THIS_MODULE,
};

struct config_group *sd_default_groups[] = {
	&extension_group,
	NULL,
};

static struct packages sdcardfs_packages = {
	.subsystem = {
		.su_group = {
			.cg_item = {
				.ci_namebuf = "sdcardfs",
				.ci_type = &packages_type,
			},
			.default_groups = sd_default_groups,
		},
	},
};

static int configfs_sdcardfs_init(void)
{
	int ret, i;
	struct configfs_subsystem *subsys = &sdcardfs_packages.subsystem;
	for (i = 0; sd_default_groups[i]; i++) {
		config_group_init(sd_default_groups[i]);
	}
	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);
	ret = configfs_register_subsystem(subsys);
	if (ret) {
		printk(KERN_ERR "Error %d while registering subsystem %s\n",
		       ret,
		       subsys->su_group.cg_item.ci_namebuf);
	}
	return ret;
}

static void configfs_sdcardfs_exit(void)
{
	configfs_unregister_subsystem(&sdcardfs_packages.subsystem);
}

int packagelist_init(void)
{
	hashtable_entry_cachep =
		kmem_cache_create("packagelist_hashtable_entry",
					sizeof(struct hashtable_entry), 0, 0, NULL);
	if (!hashtable_entry_cachep) {
		printk(KERN_ERR "sdcardfs: failed creating pkgl_hashtable entry slab cache\n");
		return -ENOMEM;
	}

	configfs_sdcardfs_init();
        return 0;
}

void packagelist_exit(void)
{
	configfs_sdcardfs_exit();
	packagelist_destroy();
	if (hashtable_entry_cachep)
		kmem_cache_destroy(hashtable_entry_cachep);
}
