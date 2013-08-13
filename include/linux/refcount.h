/*
 * Class-based reference counting
 * Its aim is to allow refcounted objects to be kept in memory even if a wrong
 * code path accidentally removes all its references while it still has users.
 * Its intention is also to make debugging easier by providing direct info about
 * the code path that brought to an incorrect put().
 * Usage:
 * 1) Create a refcount with refcount_get().
 * 2) Get refcount class with refcount_class_get() and save the pointer.
 * 3) Increase and decrease the class with refcount_inc(), refcount_dec() and
 *    refcount_dec_and_test().
 * 4) Read the global refcount with refcount_read().
 * 5) Read the class' refcount with refcount_class_read().
 * 6) Deallocate the refcount with refcount_destroy() when you need to.
 * 7) Watch the dmesg!
 * 8) Have fun finding imbalances.
 */

#ifndef _LINUX_REFCOUNT_H_
#define _LINUX_REFCOUNT_H_

#define REFCOUNT_KEY_MAX	(20)

/*
 * refcount_class_t - refs taken on an object with a given id key
 */
struct refcount_class_t {
	struct hlist_node class_node;
	char key[REFCOUNT_KEY_MAX];
	atomic_t ref;
	/* FIXME: Action is ignored by now */
	void *action;
	struct refcount_t *global;
};

/*
 * refcount_t - refs taken on an object
 */
struct refcount_t {
	/* XXX Does using a global refcount have impact on performance? */
	atomic_t ref;
	struct hlist_head table;
};

static inline void refcount_warn (char *key)
{
	printk(KERN_CRIT "BUG: refcount imbalance on key %s !", key);
	__WARN();
}

/*
 * FIXME: handling functions assume that refcounted object is already
 *        locked when a reference is taken on it - no locking is implemented
 *        here! Very carefully check code paths when calling these.
 */

/*
 * Useful references:
 * http://lxr.linux.no/#linux+v3.6.3/include/linux/list.h
 * http://lxr.linux.no/#linux+v3.6.3/arch/x86/include/asm/atomic.h
 */

/*
 * XXX Are kalloc() and kfree() really necessary, or is it more efficient
 *     to just embed refcount_t into the refcounted object?
 *     Must be sure to deallocate the dynamic classes on exit, though.
 */

static inline struct refcount_t *refcount_get (void)
{
	struct refcount_t *rc = kzalloc(sizeof(struct refcount_t), GFP_KERNEL);
	atomic_set(&rc->ref, 0);
	INIT_HLIST_HEAD(&rc->table);
	return rc;
}

static inline struct refcount_class_t *
refcount_class_init (struct refcount_t *parent, char key[REFCOUNT_KEY_MAX])
{
	struct refcount_class_t *rcc = kzalloc(sizeof(struct refcount_class_t),
						   GFP_KERNEL);
	atomic_set(&rcc->ref, 0);
	INIT_HLIST_NODE(&rcc->class_node);
	rcc->global = parent;
	strncpy(rcc->key, key, REFCOUNT_KEY_MAX);
	return rcc;
}

static inline struct refcount_class_t *
refcount_class_get (struct refcount_t *rc, char key[REFCOUNT_KEY_MAX])
{
	struct refcount_class_t *rcc;

	hlist_for_each_entry(rcc, &rc->table, class_node)
		if (!strncmp(rcc->key, key, REFCOUNT_KEY_MAX))
			return rcc;
	rcc = refcount_class_init(rc, key);
	hlist_add_head(&rcc->class_node, &rc->table);
	return rcc;
}

static inline void refcount_destroy (struct refcount_t *rc)
{
	struct refcount_class_t *rcc;
	struct hlist_node *n;

	hlist_for_each_entry_safe(rcc, n, &rc->table, class_node)
		kfree(rcc);
	kfree(rc);
}

static inline int refcount_read (struct refcount_t *rc)
{
	return atomic_read(&rc->ref);
}

static inline int refcount_class_read (struct refcount_class_t *rcc)
{
	return atomic_read(&rcc->ref);
}

/*
 * The *inc and *dec* functions take as a parameter a class, but their name
 * is generic because we assume the user wants to operate on a class but get
 * the global refcount of the object.
 */

static inline void refcount_inc (struct refcount_class_t *rcc)
{
	atomic_inc(&rcc->ref);
	atomic_inc(&rcc->global->ref);
}

static inline void refcount_add (int source,
				 struct refcount_class_t *dest)
{
	atomic_add(source, &dest->ref);
	atomic_add(source, &dest->global->ref);
}

/*
 * FIXME: by now, the action is implemented just as a simple WARN. We should
 *        execute the action decided by the implementor, instead.
 * We don't destroy classes when they reach zero, as it would be a waste
 * of time if global doesn't reach zero, and we assume we have few keys for
 * a single refcount.
 */
static inline void refcount_dec (struct refcount_class_t *rcc)
{
	atomic_dec(&rcc->ref);
	/*
	 * If the refcount class is being decremented by mistake, just
	 * exit without decreasing the global refcount, to avoid that the object
	 * is freed from user logic while it still has users. This would most
	 * likely lead to memory corruption.
	 * Be sure to warn about the issue, though.
	 */
	if (refcount_class_read(rcc) < 0) {
		refcount_warn(rcc->key);
		goto out;
	}
	BUG_ON(refcount_read(rcc->global) <= 0);
	atomic_dec(&rcc->global->ref);
out:
	return;
}

static inline bool refcount_dec_and_test (struct refcount_class_t *rcc)
{
	refcount_dec(rcc);
	return refcount_read(rcc->global) == 0;
}

#endif /* _LINUX_REFCOUNT_H_ */
