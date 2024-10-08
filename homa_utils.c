// SPDX-License-Identifier: BSD-2-Clause

/* This file contains miscellaneous utility functions for the Homa protocol. */

#include "homa_impl.h"

/* Core-specific information. NR_CPUS is an overestimate of the actual
 * number, but allows us to allocate the array statically.
 */
struct homa_core *homa_cores[NR_CPUS];

/* Information specific to individual NUMA nodes. */
struct homa_numa *homa_numas[MAX_NUMNODES];

/* Total number of  NUMA nodes actually defined in homa_numas. */
int homa_num_numas;

/* Points to block of memory holding all homa_cores; used to free it. */
char *core_memory;

struct completion homa_pacer_kthread_done;

/**
 * homa_init() - Constructor for homa objects.
 * @homa:   Object to initialize.
 *
 * Return:  0 on success, or a negative errno if there was an error. Even
 *          if an error occurs, it is safe (and necessary) to call
 *          homa_destroy at some point.
 */
int homa_init(struct homa *homa)
{
	size_t aligned_size;
	char *first;
	int i, err, num_numas;

	_Static_assert(HOMA_MAX_PRIORITIES >= 8,
		       "homa_init assumes at least 8 priority levels");

	/* Initialize data specific to NUMA nodes. */
	memset(homa_numas, 0, sizeof(homa_numas));
	num_numas = 0;
	for (i = 0; i < nr_cpu_ids; i++) {
		struct homa_numa *numa;
		int n = cpu_to_node(i);

		if (homa_numas[n])
			continue;
		numa = kmalloc(sizeof(struct homa_numa), GFP_KERNEL);
		homa_numas[n] = numa;
		homa_skb_page_pool_init(&numa->page_pool);
		if (n >= homa_num_numas)
			homa_num_numas = n+1;
		num_numas++;
	}
	pr_notice("Homa initialized %d homa_numas, highest number %d\n", num_numas, homa_num_numas-1);

	/* Initialize core-specific info (if no-one else has already done it),
	 * making sure that each core has private cache lines.
	 */
	if (!core_memory) {
		aligned_size = (sizeof(struct homa_core) + 0x3f) & ~0x3f;
		core_memory = vmalloc(0x3f + (nr_cpu_ids*aligned_size));
		if (!core_memory) {
			pr_err("Homa couldn't allocate memory for core-specific data\n");
			return -ENOMEM;
		}
		first = (char *) (((__u64) core_memory + 0x3f) & ~0x3f);
		for (i = 0; i < nr_cpu_ids; i++) {
			struct homa_core *core;
			int j;

			core = (struct homa_core *) (first + i*aligned_size);
			homa_cores[i] = core;
			core->numa = homa_numas[cpu_to_node(i)];
			core->last_active = 0;
			core->last_gro = 0;
			atomic_set(&core->softirq_backlog, 0);
			core->softirq_offset = 0;
			core->gen3_softirq_cores[0] = i^1;
			for (j = 1; j < NUM_GEN3_SOFTIRQ_CORES; j++)
				core->gen3_softirq_cores[j] = -1;
			core->last_app_active = 0;
			core->held_skb = NULL;
			core->held_bucket = 0;
			core->rpcs_locked = 0;
			core->skb_page = NULL;
			core->page_inuse = 0;
			core->page_size = 0;
			core->num_stashed_pages = 0;
		}
	}

	homa->pacer_kthread = NULL;
	init_completion(&homa_pacer_kthread_done);
	atomic64_set(&homa->next_outgoing_id, 2);
	atomic64_set(&homa->link_idle_time, get_cycles());
	spin_lock_init(&homa->grantable_lock);
	homa->grantable_lock_time = 0;
	atomic_set(&homa->grant_recalc_count, 0);
	INIT_LIST_HEAD(&homa->grantable_peers);
	INIT_LIST_HEAD(&homa->grantable_rpcs);
	homa->num_grantable_rpcs = 0;
	homa->last_grantable_change = get_cycles();
	homa->max_grantable_rpcs = 0;
	homa->oldest_rpc = NULL;
	homa->num_active_rpcs = 0;
	for (i = 0; i < HOMA_MAX_GRANTS; i++) {
		homa->active_rpcs[i] = NULL;
		atomic_set(&homa->active_remaining[i], 0);
	}
	homa->grant_nonfifo = 0;
	homa->grant_nonfifo_left = 0;
	spin_lock_init(&homa->pacer_mutex);
	homa->pacer_fifo_fraction = 50;
	homa->pacer_fifo_count = 1;
	homa->pacer_wake_time = 0;
	spin_lock_init(&homa->throttle_lock);
	INIT_LIST_HEAD_RCU(&homa->throttled_rpcs);
	homa->throttle_add = 0;
	homa->throttle_min_bytes = 200;
	atomic_set(&homa->total_incoming, 0);
	homa->next_client_port = HOMA_MIN_DEFAULT_PORT;
	homa_socktab_init(&homa->port_map);
	err = homa_peertab_init(&homa->peers);
	if (err) {
		pr_err("Couldn't initialize peer table (errno %d)\n", -err);
		return err;
	}
	spin_lock_init(&homa->page_pool_mutex);
	homa->skb_page_frees_per_sec = 1000;
	homa->skb_pages_to_free = NULL;
	homa->pages_to_free_slots = 0;
	homa->skb_page_free_time = 0;
	homa->skb_page_pool_min_kb = (3*HOMA_MAX_MESSAGE_LENGTH)/1000;

	/* Wild guesses to initialize configuration values... */
	homa->unsched_bytes = 10000;
	homa->window_param = 10000;
	homa->link_mbps = 25000;
	homa->poll_usecs = 50;
	homa->num_priorities = HOMA_MAX_PRIORITIES;
	for (i = 0; i < HOMA_MAX_PRIORITIES; i++)
		homa->priority_map[i] = i;
	homa->max_sched_prio = HOMA_MAX_PRIORITIES - 5;
	homa->unsched_cutoffs[HOMA_MAX_PRIORITIES-1] = 200;
	homa->unsched_cutoffs[HOMA_MAX_PRIORITIES-2] = 2800;
	homa->unsched_cutoffs[HOMA_MAX_PRIORITIES-3] = 15000;
	homa->unsched_cutoffs[HOMA_MAX_PRIORITIES-4] = HOMA_MAX_MESSAGE_LENGTH;
#ifdef __UNIT_TEST__
	/* Unit tests won't send CUTOFFS messages unless the test changes
	 * this variable.
	 */
	homa->cutoff_version = 0;
#else
	homa->cutoff_version = 1;
#endif
	homa->fifo_grant_increment = 10000;
	homa->grant_fifo_fraction = 50;
	homa->max_overcommit = 8;
	homa->max_incoming = 400000;
	homa->max_rpcs_per_peer = 1;
	homa->resend_ticks = 5;
	homa->resend_interval = 5;
	homa->timeout_ticks = 100;
	homa->timeout_resends = 5;
	homa->request_ack_ticks = 2;
	homa->reap_limit = 10;
	homa->dead_buffs_limit = 5000;
	homa->max_dead_buffs = 0;
	homa->pacer_kthread = kthread_run(homa_pacer_main, homa,
			"homa_pacer");
	if (IS_ERR(homa->pacer_kthread)) {
		err = PTR_ERR(homa->pacer_kthread);
		homa->pacer_kthread = NULL;
		pr_err("couldn't create homa pacer thread: error %d\n", err);
		return err;
	}
	homa->pacer_exit = false;
	homa->max_nic_queue_ns = 2000;
	homa->cycles_per_kbyte = 0;
	homa->verbose = 0;
	homa->max_gso_size = 10000;
	homa->gso_force_software = 0;
	homa->hijack_tcp = 0;
	homa->max_gro_skbs = 20;
	homa->gro_policy = HOMA_GRO_NORMAL;
	homa->busy_usecs = 100;
	homa->gro_busy_usecs = 5;
	homa->timer_ticks = 0;
	spin_lock_init(&homa->metrics_lock);
	homa->metrics = NULL;
	homa->metrics_capacity = 0;
	homa->metrics_length = 0;
	homa->metrics_active_opens = 0;
	homa->flags = 0;
	homa->freeze_type = 0;
	homa->bpage_lease_usecs = 10000;
	homa->next_id = 0;
	homa_outgoing_sysctl_changed(homa);
	homa_incoming_sysctl_changed(homa);
	return 0;
}

/**
 * homa_destroy() -  Destructor for homa objects.
 * @homa:      Object to destroy.
 */
void homa_destroy(struct homa *homa)
{
	int i;

	if (homa->pacer_kthread) {
		homa_pacer_stop(homa);
		wait_for_completion(&homa_pacer_kthread_done);
	}

	/* The order of the following 2 statements matters! */
	homa_socktab_destroy(&homa->port_map);
	homa_peertab_destroy(&homa->peers);
	homa_skb_cleanup(homa);

	for (i = 0; i < MAX_NUMNODES; i++) {
		struct homa_numa *numa = homa_numas[i];

		if (numa != NULL) {
			kfree(numa);
			homa_numas[i] = NULL;
		}
	}
	if (core_memory) {
		vfree(core_memory);
		core_memory = NULL;
		for (i = 0; i < nr_cpu_ids; i++)
			homa_cores[i] = NULL;
	}
	kfree(homa->metrics);
}

/**
 * homa_rpc_new_client() - Allocate and construct a client RPC (one that is used
 * to issue an outgoing request). Doesn't send any packets. Invoked with no
 * locks held.
 * @hsk:      Socket to which the RPC belongs.
 * @dest:     Address of host (ip and port) to which the RPC will be sent.
 *
 * Return:    A printer to the newly allocated object, or a negative
 *            errno if an error occurred. The RPC will be locked; the
 *            caller must eventually unlock it.
 */
struct homa_rpc *homa_rpc_new_client(struct homa_sock *hsk,
		const union sockaddr_in_union *dest)
{
	int err;
	struct homa_rpc *crpc;
	struct homa_rpc_bucket *bucket;
	struct in6_addr dest_addr_as_ipv6 = canonical_ipv6_addr(dest);

	crpc = kmalloc(sizeof(*crpc), GFP_KERNEL);
	if (unlikely(!crpc))
		return ERR_PTR(-ENOMEM);

	/* Initialize fields that don't require the socket lock. */
	crpc->hsk = hsk;
	crpc->id = atomic64_fetch_add(2, &hsk->homa->next_outgoing_id);
	bucket = homa_client_rpc_bucket(hsk, crpc->id);
	crpc->bucket = bucket;
	crpc->state = RPC_OUTGOING;
	atomic_set(&crpc->flags, 0);
	atomic_set(&crpc->grants_in_progress, 0);
	crpc->peer = homa_peer_find(&hsk->homa->peers, &dest_addr_as_ipv6,
			&hsk->inet);
	if (IS_ERR(crpc->peer)) {
		tt_record("error in homa_peer_find");
		err = PTR_ERR(crpc->peer);
		goto error;
	}
	crpc->dport = ntohs(dest->in6.sin6_port);
	crpc->completion_cookie = 0;
	crpc->error = 0;
	crpc->msgin.length = -1;
	crpc->msgin.num_bpages = 0;
	memset(&crpc->msgout, 0, sizeof(crpc->msgout));
	crpc->msgout.length = -1;
	INIT_LIST_HEAD(&crpc->ready_links);
	INIT_LIST_HEAD(&crpc->buf_links);
	INIT_LIST_HEAD(&crpc->dead_links);
	crpc->interest = NULL;
	INIT_LIST_HEAD(&crpc->grantable_links);
	INIT_LIST_HEAD(&crpc->throttled_links);
	crpc->silent_ticks = 0;
	crpc->resend_timer_ticks = hsk->homa->timer_ticks;
	crpc->done_timer_ticks = 0;
	crpc->magic = HOMA_RPC_MAGIC;
	crpc->start_cycles = get_cycles();

	/* Initialize fields that require locking. This allows the most
	 * expensive work, such as copying in the message from user space,
	 * to be performed without holding locks. Also, can't hold spin
	 * locks while doing things that could block, such as memory allocation.
	 */
	homa_bucket_lock(bucket, crpc->id, "homa_rpc_new_client");
	homa_sock_lock(hsk, "homa_rpc_new_client");
	if (hsk->shutdown) {
		homa_sock_unlock(hsk);
		homa_rpc_unlock(crpc);
		err = -ESHUTDOWN;
		goto error;
	}
	hlist_add_head(&crpc->hash_links, &bucket->rpcs);
	list_add_tail_rcu(&crpc->active_links, &hsk->active_rpcs);
	homa_sock_unlock(hsk);

	return crpc;

error:
	kfree(crpc);
	return ERR_PTR(err);
}

/**
 * homa_rpc_new_server() - Allocate and construct a server RPC (one that is
 * used to manage an incoming request). If appropriate, the RPC will also
 * be handed off (we do it here, while we have the socket locked, to avoid
 * acquiring the socket lock a second time later for the handoff).
 * @hsk:      Socket that owns this RPC.
 * @source:   IP address (network byte order) of the RPC's client.
 * @h:        Header for the first data packet received for this RPC; used
 *            to initialize the RPC.
 * @created:  Will be set to 1 if a new RPC was created and 0 if an
 *            existing RPC was found.
 *
 * Return:  A pointer to a new RPC, which is locked, or a negative errno
 *          if an error occurred. If there is already an RPC corresponding
 *          to h, then it is returned instead of creating a new RPC.
 */
struct homa_rpc *homa_rpc_new_server(struct homa_sock *hsk,
		const struct in6_addr *source, struct data_header *h,
		int *created)
{
	int err;
	struct homa_rpc *srpc = NULL;
	__u64 id = homa_local_id(h->common.sender_id);
	struct homa_rpc_bucket *bucket = homa_server_rpc_bucket(hsk, id);

	/* Lock the bucket, and make sure no-one else has already created
	 * the desired RPC.
	 */
	homa_bucket_lock(bucket, id, "homa_rpc_new_server");
	hlist_for_each_entry_rcu(srpc, &bucket->rpcs, hash_links) {
		if ((srpc->id == id) &&
				(srpc->dport == ntohs(h->common.sport)) &&
				ipv6_addr_equal(&srpc->peer->addr, source)) {
			/* RPC already exists; just return it instead
			 * of creating a new RPC.
			 */
			*created = 0;
			return srpc;
		}
	}

	/* Initialize fields that don't require the socket lock. */
	srpc = kmalloc(sizeof(*srpc), GFP_KERNEL);
	if (!srpc) {
		err = -ENOMEM;
		goto error;
	}
	srpc->hsk = hsk;
	srpc->bucket = bucket;
	srpc->state = RPC_INCOMING;
	atomic_set(&srpc->flags, 0);
	atomic_set(&srpc->grants_in_progress, 0);
	srpc->peer = homa_peer_find(&hsk->homa->peers, source, &hsk->inet);
	if (IS_ERR(srpc->peer)) {
		err = PTR_ERR(srpc->peer);
		goto error;
	}
	srpc->dport = ntohs(h->common.sport);
	srpc->id = id;
	srpc->completion_cookie = 0;
	srpc->error = 0;
	srpc->msgin.length = -1;
	srpc->msgin.num_bpages = 0;
	memset(&srpc->msgout, 0, sizeof(srpc->msgout));
	srpc->msgout.length = -1;
	INIT_LIST_HEAD(&srpc->ready_links);
	INIT_LIST_HEAD(&srpc->buf_links);
	INIT_LIST_HEAD(&srpc->dead_links);
	srpc->interest = NULL;
	INIT_LIST_HEAD(&srpc->grantable_links);
	INIT_LIST_HEAD(&srpc->throttled_links);
	srpc->silent_ticks = 0;
	srpc->resend_timer_ticks = hsk->homa->timer_ticks;
	srpc->done_timer_ticks = 0;
	srpc->magic = HOMA_RPC_MAGIC;
	srpc->start_cycles = get_cycles();
	tt_record2("Incoming message for id %d has %d unscheduled bytes",
			srpc->id, ntohl(h->incoming));
	err = homa_message_in_init(srpc, ntohl(h->message_length),
			ntohl(h->incoming));
	if (err != 0)
		goto error;

	/* Initialize fields that require socket to be locked. */
	homa_sock_lock(hsk, "homa_rpc_new_server");
	if (hsk->shutdown) {
		homa_sock_unlock(hsk);
		err = -ESHUTDOWN;
		goto error;
	}
	hlist_add_head(&srpc->hash_links, &bucket->rpcs);
	list_add_tail_rcu(&srpc->active_links, &hsk->active_rpcs);
	if ((ntohl(h->seg.offset) == 0) && (srpc->msgin.num_bpages > 0)) {
		atomic_or(RPC_PKTS_READY, &srpc->flags);
		homa_rpc_handoff(srpc);
	}
	homa_sock_unlock(hsk);
	INC_METRIC(requests_received, 1);
	*created = 1;
	return srpc;

error:
	homa_bucket_unlock(bucket, id);
	kfree(srpc);
	return ERR_PTR(err);
}

/**
 * homa_bucket_lock_slow() - This function implements the slow path for
 * locking a bucket in one of the hash tables of RPCs. It is invoked when a
 * lock isn't immediately available. It waits for the lock, but also records
 * statistics about the waiting time.
 * @bucket:    The hash table bucket to lock.
 * @id:        ID of the particular RPC being locked (multiple RPCs may
 *             share a single bucket lock).
 */
void homa_bucket_lock_slow(struct homa_rpc_bucket *bucket, __u64 id)
{
	__u64 start = get_cycles();

	tt_record2("beginning wait for rpc lock, id %d (bucket %d)",
			id, bucket->id);
	spin_lock_bh(&bucket->lock);
	tt_record2("ending wait for bucket lock, id %d (bucket %d)",
			id, bucket->id);
	if (homa_is_client(id)) {
		INC_METRIC(client_lock_misses, 1);
		INC_METRIC(client_lock_miss_cycles, get_cycles() - start);
	} else {
		INC_METRIC(server_lock_misses, 1);
		INC_METRIC(server_lock_miss_cycles, get_cycles() - start);
	}
}

/**
 * homa_rpc_acked() - This function is invoked when an ack is received
 * for an RPC; if the RPC still exists, is freed.
 * @hsk:     Socket on which the ack was received. May or may not correspond
 *           to the RPC, but can sometimes be used to avoid a socket lookup.
 * @saddr:   Source address from which the act was received (the client
 *           note for the RPC)
 * @ack:     Information about an RPC from @saddr that may now be deleted safely.
 */
void homa_rpc_acked(struct homa_sock *hsk, const struct in6_addr *saddr,
		struct homa_ack *ack)
{
	struct homa_rpc *rpc;
	struct homa_sock *hsk2 = hsk;
	__u64 id = homa_local_id(ack->client_id);
	__u16 client_port = ntohs(ack->client_port);
	__u16 server_port = ntohs(ack->server_port);

	UNIT_LOG("; ", "ack %llu", id);
	if (hsk2->port != server_port) {
		/* Without RCU, sockets other than hsk can be deleted
		 * out from under us.
		 */
		rcu_read_lock();
		hsk2 = homa_sock_find(&hsk->homa->port_map, server_port);
		if (!hsk2)
			goto done;
	}
	rpc = homa_find_server_rpc(hsk2, saddr, client_port, id);
	if (rpc) {
		tt_record1("homa_rpc_acked freeing id %d", rpc->id);
		homa_rpc_free(rpc);
		homa_rpc_unlock(rpc);
	}

done:
	if (hsk->port != server_port)
		rcu_read_unlock();
}

/**
 * homa_rpc_free() - Destructor for homa_rpc; will arrange for all resources
 * associated with the RPC to be released (eventually).
 * @rpc:  Structure to clean up, or NULL. Must be locked. Its socket must
 *        not be locked.
 */
void homa_rpc_free(struct homa_rpc *rpc)
{
	/* The goal for this function is to make the RPC inaccessible,
	 * so that no other code will ever access it again. However, don't
	 * actually release resources; leave that to homa_rpc_reap, which
	 * runs later. There are two reasons for this. First, releasing
	 * resources may be expensive, so we don't want to keep the caller
	 * waiting; homa_rpc_reap will run in situations where there is time
	 * to spare. Second, there may be other code that currently has
	 * pointers to this RPC but temporarily released the lock (e.g. to
	 * copy data to/from user space). It isn't safe to clean up until
	 * that code has finished its work and released any pointers to the
	 * RPC (homa_rpc_reap will ensure that this has happened). So, this
	 * function should only make changes needed to make the RPC
	 * inaccessible.
	 */
	if (!rpc || (rpc->state == RPC_DEAD))
		return;
	UNIT_LOG("; ", "homa_rpc_free invoked");
	tt_record1("homa_rpc_free invoked for id %d", rpc->id);
	rpc->state = RPC_DEAD;

	/* The following line must occur before the socket is locked or
	 * RPC is added to dead_rpcs. This is necessary because homa_grant_free
	 * releases the RPC lock and reacquires it (see comment in
	 * homa_grant_free for more info).
	 */
	homa_grant_free_rpc(rpc);

	/* Unlink from all lists, so no-one will ever find this RPC again. */
	homa_sock_lock(rpc->hsk, "homa_rpc_free");
	__hlist_del(&rpc->hash_links);
	list_del_rcu(&rpc->active_links);
	list_add_tail_rcu(&rpc->dead_links, &rpc->hsk->dead_rpcs);
	__list_del_entry(&rpc->ready_links);
	__list_del_entry(&rpc->buf_links);
	if (rpc->interest != NULL) {
		rpc->interest->reg_rpc = NULL;
		wake_up_process(rpc->interest->thread);
		rpc->interest = NULL;
	}
//	tt_record3("Freeing rpc id %d, socket %d, dead_skbs %d", rpc->id,
//			rpc->hsk->client_port,
//			rpc->hsk->dead_skbs);

	if (rpc->msgin.length >= 0) {
		rpc->hsk->dead_skbs += skb_queue_len(&rpc->msgin.packets);
		while (1) {
			struct homa_gap *gap = list_first_entry_or_null(
					&rpc->msgin.gaps, struct homa_gap, links);
			if (gap == NULL)
				break;
			list_del(&gap->links);
			kfree(gap);
		}
	}
	rpc->hsk->dead_skbs += rpc->msgout.num_skbs;
	if (rpc->hsk->dead_skbs > rpc->hsk->homa->max_dead_buffs)
		/* This update isn't thread-safe; it's just a
		 * statistic so it's OK if updates occasionally get
		 * missed.
		 */
		rpc->hsk->homa->max_dead_buffs = rpc->hsk->dead_skbs;

	homa_sock_unlock(rpc->hsk);
	homa_remove_from_throttled(rpc);
}

/**
 * homa_rpc_reap() - Invoked to release resources associated with dead
 * RPCs for a given socket. For a large RPC, it can take a long time to
 * free all of its packet buffers, so we try to perform this work
 * off the critical path where it won't delay applications. Each call to
 * this function does a small chunk of work. See the file reap.txt for
 * more information.
 * @hsk:   Homa socket that may contain dead RPCs. Must not be locked by the
 *         caller; this function will lock and release.
 * @count: Number of buffers to free during this call.
 *
 * Return: A return value of 0 means that we ran out of work to do; calling
 *         again will do no work (there could be unreaped RPCs, but if so,
 *         reaping has been disabled for them).  A value greater than
 *         zero means there is still more reaping work to be done.
 */
int homa_rpc_reap(struct homa_sock *hsk, int count)
{
#ifdef __UNIT_TEST__
#define BATCH_MAX 3
#else
#define BATCH_MAX 20
#endif
	struct sk_buff *skbs[BATCH_MAX];
	struct homa_rpc *rpcs[BATCH_MAX];
	int num_skbs, num_rpcs;
	struct homa_rpc *rpc;
	int i, batch_size;
	int rx_frees = 0;
	int result;

	INC_METRIC(reaper_calls, 1);
	INC_METRIC(reaper_dead_skbs, hsk->dead_skbs);

	/* Each iteration through the following loop will reap
	 * BATCH_MAX skbs.
	 */
	while (count > 0) {
		batch_size = count;
		if (batch_size > BATCH_MAX)
			batch_size = BATCH_MAX;
		count -= batch_size;
		num_skbs = num_rpcs = 0;

		homa_sock_lock(hsk, "homa_rpc_reap");
		if (atomic_read(&hsk->protect_count)) {
			INC_METRIC(disabled_reaps, 1);
			tt_record2("homa_rpc_reap returning: protect_count %d, dead_skbs %d",
					atomic_read(&hsk->protect_count),
					hsk->dead_skbs);
			homa_sock_unlock(hsk);
			return 0;
		}

		/* Collect buffers and freeable RPCs. */
		list_for_each_entry_rcu(rpc, &hsk->dead_rpcs, dead_links) {
			if ((atomic_read(&rpc->flags) & RPC_CANT_REAP)
					|| (atomic_read(&rpc->grants_in_progress)
					!= 0)
					|| (atomic_read(&rpc->msgout.active_xmits)
					!= 0)) {
				INC_METRIC(disabled_rpc_reaps, 1);
				continue;
			}
			rpc->magic = 0;

			/* For Tx sk_buffs, collect them here but defer
			 * freeing until after releasing the socket lock.
			 */
			if (rpc->msgout.length >= 0) {
				while (rpc->msgout.packets) {
					skbs[num_skbs] = rpc->msgout.packets;
					rpc->msgout.packets = homa_get_skb_info(
							rpc->msgout.packets)
							->next_skb;
					num_skbs++;
					rpc->msgout.num_skbs--;
					if (num_skbs >= batch_size)
						goto release;
				}
			}

			/* In the normal case rx sk_buffs will already have been
			 * freed before we got here. Thus it's OK to free
			 * immediately in rare situations where there are
			 * buffers left.
			 */
			if (rpc->msgin.length >= 0) {
				while (1) {
					struct sk_buff *skb;

					skb = skb_dequeue(&rpc->msgin.packets);
					if (!skb)
						break;
					kfree_skb(skb);
					rx_frees++;
				}
			}

			/* If we get here, it means all packets have been
			 *  removed from the RPC.
			 */
			rpcs[num_rpcs] = rpc;
			num_rpcs++;
			list_del_rcu(&rpc->dead_links);
			if (num_rpcs >= batch_size)
				goto release;
		}

		/* Free all of the collected resources; release the socket
		 * lock while doing this.
		 */
release:
		hsk->dead_skbs -= num_skbs + rx_frees;
		result = !list_empty(&hsk->dead_rpcs)
				&& ((num_skbs + num_rpcs) != 0);
		homa_sock_unlock(hsk);
		homa_skb_free_many_tx(hsk->homa, skbs, num_skbs);
		for (i = 0; i < num_rpcs; i++) {
			rpc = rpcs[i];
			UNIT_LOG("; ", "reaped %llu", rpc->id);
			/* Lock and unlock the RPC before freeing it. This
			 * is needed to deal with races where the code
			 * that invoked homa_rpc_free hasn't unlocked the
			 * RPC yet.
			 */
			homa_rpc_lock(rpc, "homa_rpc_reap");
			homa_rpc_unlock(rpc);

			if (unlikely(rpc->msgin.num_bpages))
				homa_pool_release_buffers(
						&rpc->hsk->buffer_pool,
						rpc->msgin.num_bpages,
						rpc->msgin.bpage_offsets);
			if (rpc->msgin.length >= 0) {
				while (1) {
					struct homa_gap *gap = list_first_entry_or_null(
							&rpc->msgin.gaps,
							struct homa_gap, links);
					if (gap == NULL)
						break;
					list_del(&gap->links);
					kfree(gap);
				}
			}
			tt_record1("homa_rpc_reap finished reaping id %d",
					rpc->id);
			rpc->state = 0;
			kfree(rpc);
		}
		tt_record4("reaped %d skbs, %d rpcs; %d skbs remain for port %d",
				num_skbs + rx_frees, num_rpcs, hsk->dead_skbs,
				hsk->port);
		if (!result)
			break;
	}
	homa_pool_check_waiting(&hsk->buffer_pool);
	return result;
}

/**
 * homa_find_client_rpc() - Locate client-side information about the RPC that
 * a packet belongs to, if there is any. Thread-safe without socket lock.
 * @hsk:      Socket via which packet was received.
 * @id:       Unique identifier for the RPC.
 *
 * Return:    A pointer to the homa_rpc for this id, or NULL if none.
 *            The RPC will be locked; the caller must eventually unlock it
 *            by invoking homa_rpc_unlock.
 */
struct homa_rpc *homa_find_client_rpc(struct homa_sock *hsk, __u64 id)
{
	struct homa_rpc *crpc;
	struct homa_rpc_bucket *bucket = homa_client_rpc_bucket(hsk, id);

	homa_bucket_lock(bucket, id, __func__);
	hlist_for_each_entry_rcu(crpc, &bucket->rpcs, hash_links) {
		if (crpc->id == id)
			return crpc;
	}
	homa_bucket_unlock(bucket, id);
	return NULL;
}

/**
 * homa_find_server_rpc() - Locate server-side information about the RPC that
 * a packet belongs to, if there is any. Thread-safe without socket lock.
 * @hsk:      Socket via which packet was received.
 * @saddr:    Address from which the packet was sent.
 * @sport:    Port at @saddr from which the packet was sent.
 * @id:       Unique identifier for the RPC (must have server bit set).
 *
 * Return:    A pointer to the homa_rpc matching the arguments, or NULL
 *            if none. The RPC will be locked; the caller must eventually
 *            unlock it by invoking homa_rpc_unlock.
 */
struct homa_rpc *homa_find_server_rpc(struct homa_sock *hsk,
		const struct in6_addr *saddr, __u16 sport, __u64 id)
{
	struct homa_rpc *srpc;
	struct homa_rpc_bucket *bucket = homa_server_rpc_bucket(hsk, id);

	homa_bucket_lock(bucket, id, __func__);
	hlist_for_each_entry_rcu(srpc, &bucket->rpcs, hash_links) {
		if ((srpc->id == id) && (srpc->dport == sport) &&
				ipv6_addr_equal(&srpc->peer->addr, saddr))
			return srpc;
	}
	homa_bucket_unlock(bucket, id);
	return NULL;
}

/**
 * homa_rpc_log() - Log info about a particular RPC; this is functionality
 * pulled out of homa_rpc_log_active because its indentation got too deep.
 * @rpc:  RPC for which key info should be written to the system log.
 */
void homa_rpc_log(struct homa_rpc *rpc)
{
	char *type = homa_is_client(rpc->id) ? "Client" : "Server";
	char *peer = homa_print_ipv6_addr(&rpc->peer->addr);

	if (rpc->state == RPC_INCOMING)
		pr_notice("%s RPC INCOMING, id %llu, peer %s:%d, %d/%d bytes received, incoming %d\n",
				type, rpc->id, peer, rpc->dport,
				rpc->msgin.length
				- rpc->msgin.bytes_remaining,
				rpc->msgin.length, rpc->msgin.granted);
	else if (rpc->state == RPC_OUTGOING) {
		pr_notice("%s RPC OUTGOING, id %llu, peer %s:%d, out length %d, left %d, granted %d, in left %d, resend_ticks %u, silent_ticks %d\n",
				type, rpc->id, peer, rpc->dport,
				rpc->msgout.length,
				rpc->msgout.length - rpc->msgout.next_xmit_offset,
				rpc->msgout.granted,
				rpc->msgin.bytes_remaining,
				rpc->resend_timer_ticks,
				rpc->silent_ticks);
	} else {
		pr_notice("%s RPC %s, id %llu, peer %s:%d, incoming length %d, outgoing length %d\n",
				type, homa_symbol_for_state(rpc),
				rpc->id, peer, rpc->dport,
				rpc->msgin.length, rpc->msgout.length);
	}
}

/**
 * homa_rpc_log_active() - Print information to the system log about all
 * active RPCs. Intended primarily for debugging.
 * @homa:    Overall data about the Homa protocol implementation.
 * @id:      An RPC id: if nonzero, then only RPCs with this id will be
 *           logged.
 */
void homa_rpc_log_active(struct homa *homa, uint64_t id)
{
	struct homa_socktab_scan scan;
	struct homa_sock *hsk;
	struct homa_rpc *rpc;
	int count = 0;

	pr_notice("Logging active Homa RPCs:\n");
	rcu_read_lock();
	for (hsk = homa_socktab_start_scan(&homa->port_map, &scan);
			hsk !=  NULL; hsk = homa_socktab_next(&scan)) {
		if (list_empty(&hsk->active_rpcs) || hsk->shutdown)
			continue;

		if (!homa_protect_rpcs(hsk))
			continue;
		list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links) {
			count++;
			if ((id != 0) && (id != rpc->id))
				continue;
			homa_rpc_log(rpc);
					}
		homa_unprotect_rpcs(hsk);
	}
	rcu_read_unlock();
	pr_notice("Finished logging active Homa RPCs: %d active RPCs\n", count);
}

/**
 * homa_rpc_log_tt() - Log info about a particular RPC using timetraces.
 * @rpc:  RPC for which key info should be written to the system log.
 */
void homa_rpc_log_tt(struct homa_rpc *rpc)
{
	if (rpc->state == RPC_INCOMING) {
		int received = rpc->msgin.length
				- rpc->msgin.bytes_remaining;
		tt_record4("Incoming RPC id %d, peer 0x%x, %d/%d bytes received",
				rpc->id, tt_addr(rpc->peer->addr),
				received, rpc->msgin.length);
		if (1)
			tt_record4("RPC id %d has incoming %d, granted %d, prio %d", rpc->id,
					rpc->msgin.granted - received,
					rpc->msgin.granted, rpc->msgin.priority);
		tt_record4("RPC id %d: length %d, remaining %d, rank %d",
				rpc->id, rpc->msgin.length,
				rpc->msgin.bytes_remaining,
				atomic_read(&rpc->msgin.rank));
		if (rpc->msgin.num_bpages == 0)
			tt_record1("RPC id %d is blocked waiting for buffers",
					rpc->id);
		else
			tt_record2("RPC id %d has %d bpages allocated",
					rpc->id, rpc->msgin.num_bpages);
	} else if (rpc->state == RPC_OUTGOING) {
		tt_record4("Outgoing RPC id %d, peer 0x%x, %d/%d bytes sent",
				rpc->id, tt_addr(rpc->peer->addr),
				rpc->msgout.next_xmit_offset,
				rpc->msgout.length);
		if (rpc->msgout.granted > rpc->msgout.next_xmit_offset)
			tt_record3("RPC id %d has %d unsent grants (granted %d)",
					rpc->id, rpc->msgout.granted
					- rpc->msgout.next_xmit_offset,
					rpc->msgout.granted);
	} else {
		tt_record2("RPC id %d is in state %d", rpc->id, rpc->state);
	}
}

/**
 * homa_rpc_log_active_tt() - Log information about all active RPCs using
 * timetraces.
 * @homa:    Overall data about the Homa protocol implementation.
 * @freeze_count:  If nonzero, FREEZE requests will be sent for this many
 *                 incoming RPCs with outstanding grants
 */
void homa_rpc_log_active_tt(struct homa *homa, int freeze_count)
{
	struct homa_socktab_scan scan;
	struct homa_sock *hsk;
	struct homa_rpc *rpc;
	int count = 0;

	homa_grant_log_tt(homa);
	tt_record("Logging active Homa RPCs:");
	rcu_read_lock();
	for (hsk = homa_socktab_start_scan(&homa->port_map, &scan);
			hsk !=  NULL; hsk = homa_socktab_next(&scan)) {
		if (list_empty(&hsk->active_rpcs) || hsk->shutdown)
			continue;

		if (!homa_protect_rpcs(hsk))
			continue;
		list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links) {
			struct freeze_header freeze;

			count++;
			homa_rpc_log_tt(rpc);
			if (freeze_count == 0)
				continue;
			if (rpc->state != RPC_INCOMING)
				continue;
			if (rpc->msgin.granted <= (rpc->msgin.length
					- rpc->msgin.bytes_remaining))
				continue;
			freeze_count--;
			pr_notice("Emitting FREEZE in %s\n", __func__);
			homa_xmit_control(FREEZE, &freeze, sizeof(freeze), rpc);
		}
		homa_unprotect_rpcs(hsk);
	}
	rcu_read_unlock();
	tt_record1("Finished logging (%d active Homa RPCs)", count);
}

/**
 * homa_validate_incoming() - Scan all of the active RPCs to compute what
 * homa_total_incoming should be, and see if it actually matches.
 * @homa:         Overall data about the Homa protocol implementation.
 * @verbose:      Print incoming info for each individual RPC.
 * @link_errors:  Set to 1 if one or more grantable RPCs don't seem to
 *                be linked into the grantable lists.
 * Return:   The difference between the actual value of homa->total_incoming
 *           and the expected value computed from the individual RPCs (positive
 *           means homa->total_incoming is higher than expected).
 */
int homa_validate_incoming(struct homa *homa, int verbose, int *link_errors)
{
	struct homa_socktab_scan scan;
	struct homa_sock *hsk;
	struct homa_rpc *rpc;
	int total_incoming = 0;
	int actual;

	tt_record1("homa_validate_incoming starting, total_incoming %d",
			atomic_read(&homa->total_incoming));
	*link_errors = 0;
	rcu_read_lock();
	for (hsk = homa_socktab_start_scan(&homa->port_map, &scan);
			hsk !=  NULL; hsk = homa_socktab_next(&scan)) {
		if (list_empty(&hsk->active_rpcs) || hsk->shutdown)
			continue;

		if (!homa_protect_rpcs(hsk))
			continue;
		list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links) {
			int incoming;

			if (rpc->state != RPC_INCOMING)
				continue;
			incoming = rpc->msgin.granted -
					(rpc->msgin.length
					- rpc->msgin.bytes_remaining);
			if (incoming < 0)
				incoming = 0;
			if (rpc->msgin.rec_incoming == 0)
				continue;
			total_incoming += rpc->msgin.rec_incoming;
			if (verbose)
				tt_record3("homa_validate_incoming: RPC id %d, ncoming %d, rec_incoming %d",
						rpc->id, incoming,
						rpc->msgin.rec_incoming);
			if (rpc->msgin.granted >= rpc->msgin.length)
				continue;
			if (list_empty(&rpc->grantable_links)) {
				tt_record1("homa_validate_incoming: RPC id %d not linked in grantable list",
						rpc->id);
				*link_errors = 1;
			}
			if (list_empty(&rpc->grantable_links)) {
				tt_record1("homa_validate_incoming: RPC id %d peer not linked in grantable list",
						rpc->id);
				*link_errors = 1;
			}
		}
		homa_unprotect_rpcs(hsk);
	}
	rcu_read_unlock();
	actual = atomic_read(&homa->total_incoming);
	tt_record3("homa_validate_incoming diff %d (expected %d, got %d)",
			actual - total_incoming, total_incoming, actual);
	return actual - total_incoming;
}

/**
 * homa_print_ipv4_addr() - Convert an IPV4 address to the standard string
 * representation.
 * @addr:    Address to convert, in network byte order.
 *
 * Return:   The converted value. Values are stored in static memory, so
 *           the caller need not free. This also means that storage is
 *           eventually reused (there are enough buffers to accommodate
 *           multiple "active" values).
 *
 * Note: Homa uses this function, rather than the %pI4 format specifier
 * for snprintf et al., because the kernel's version of snprintf isn't
 * available in Homa's unit test environment.
 */
char *homa_print_ipv4_addr(__be32 addr)
{
#define NUM_BUFS_IPV4 4
#define BUF_SIZE_IPV4 30
	static char buffers[NUM_BUFS_IPV4][BUF_SIZE_IPV4];
	static int next_buf;
	__u32 a2 = ntohl(addr);
	char *buffer = buffers[next_buf];

	next_buf++;
	if (next_buf >= NUM_BUFS_IPV4)
		next_buf = 0;
	snprintf(buffer, BUF_SIZE_IPV4, "%u.%u.%u.%u", (a2 >> 24) & 0xff,
			(a2 >> 16) & 0xff, (a2 >> 8) & 0xff, a2 & 0xff);
	return buffer;
}

/**
 * homa_print_ipv6_addr() - Convert an IPv6 address to a human-readable string
 * representation. IPv4-mapped addresses are printed in IPv4 syntax.
 * @addr:    Address to convert, in network byte order.
 *
 * Return:   The converted value. Values are stored in static memory, so
 *           the caller need not free. This also means that storage is
 *           eventually reused (there are enough buffers to accommodate
 *           multiple "active" values).
 */
char *homa_print_ipv6_addr(const struct in6_addr *addr)
{
#define NUM_BUFS (1 << 2)
#define BUF_SIZE 64
	static char buffers[NUM_BUFS][BUF_SIZE];
	static int next_buf;
	char *buffer = buffers[next_buf];

	next_buf++;
	if (next_buf >= NUM_BUFS)
		next_buf = 0;
#ifdef __UNIT_TEST__
	struct in6_addr zero = {};

	if (ipv6_addr_equal(addr, &zero)) {
		snprintf(buffer, BUF_SIZE, "0.0.0.0");
	} else if ((addr->s6_addr32[0] == 0) &&
		(addr->s6_addr32[1] == 0) &&
		(addr->s6_addr32[2] == htonl(0x0000ffff))) {
		__u32 a2 = ntohl(addr->s6_addr32[3]);

		snprintf(buffer, BUF_SIZE, "%u.%u.%u.%u", (a2 >> 24) & 0xff,
				(a2 >> 16) & 0xff, (a2 >> 8) & 0xff, a2 & 0xff);
	} else {
		const char *inet_ntop(int af, const void *src, char *dst,
				size_t size);
		inet_ntop(AF_INET6, addr, buffer + 1, BUF_SIZE);
		buffer[0] = '[';
		strcat(buffer, "]");
	}
#else
	snprintf(buffer, BUF_SIZE, "%pI6", addr);
#endif
	return buffer;
}

/**
 * homa_print_packet() - Print a human-readable string describing the
 * information in a Homa packet.
 * @skb:     Packet whose information should be printed.
 * @buffer:  Buffer in which to generate the string.
 * @buf_len: Number of bytes available at @buffer.
 *
 * Return:   @buffer
 */
char *homa_print_packet(struct sk_buff *skb, char *buffer, int buf_len)
{
	int used = 0;
	struct common_header *common;
	struct in6_addr saddr;
	char header[HOMA_MAX_HEADER];

	if (skb == NULL) {
		snprintf(buffer, buf_len, "skb is NULL!");
		buffer[buf_len-1] = 0;
		return buffer;
	}

	homa_skb_get(skb, &header, 0, sizeof(header));
	common = (struct common_header *) header;
	saddr = skb_canonical_ipv6_saddr(skb);
	used = homa_snprintf(buffer, buf_len, used,
		"%s from %s:%u, dport %d, id %llu",
		homa_symbol_for_type(common->type),
		homa_print_ipv6_addr(&saddr),
		ntohs(common->sport), ntohs(common->dport),
		be64_to_cpu(common->sender_id));
	switch (common->type) {
	case DATA: {
		struct data_header *h = (struct data_header *) header;
		struct homa_skb_info *homa_info = homa_get_skb_info(skb);
		int data_left, i, seg_length, pos, offset;

		if (skb_shinfo(skb)->gso_segs == 0) {
			seg_length = homa_data_len(skb);
			data_left = 0;
		} else {
			seg_length = homa_info->seg_length;
			if (seg_length > homa_info->data_bytes)
				seg_length = homa_info->data_bytes;
			data_left = homa_info->data_bytes - seg_length;
		}
		offset = ntohl(h->seg.offset);
		if (offset == -1)
			offset = ntohl(h->common.sequence);
		used = homa_snprintf(buffer, buf_len, used,
				", message_length %d, offset %d, data_length %d, incoming %d",
				ntohl(h->message_length), offset,
				seg_length, ntohl(h->incoming));
		if (ntohs(h->cutoff_version != 0))
			used = homa_snprintf(buffer, buf_len, used,
					", cutoff_version %d",
					ntohs(h->cutoff_version));
		if (h->retransmit)
			used = homa_snprintf(buffer, buf_len, used,
					", RETRANSMIT");
		if (skb_shinfo(skb)->gso_type == 0xd)
			used = homa_snprintf(buffer, buf_len, used,
					", TSO disabled");
		if (skb_shinfo(skb)->gso_segs <= 1)
			break;
		pos = skb_transport_offset(skb) + sizeof32(*h) + seg_length;
		used = homa_snprintf(buffer, buf_len, used, ", extra segs");
		for (i = skb_shinfo(skb)->gso_segs - 1; i > 0; i--) {
			if (homa_info->seg_length < skb_shinfo(skb)->gso_size) {
				struct seg_header seg;

				homa_skb_get(skb, &seg, pos, sizeof(seg));
				offset = ntohl(seg.offset);
			} else {
				offset += seg_length;
			}
			if (seg_length > data_left)
				seg_length = data_left;
			used = homa_snprintf(buffer, buf_len, used,
					" %d@%d", seg_length, offset);
			data_left -= seg_length;
			pos += skb_shinfo(skb)->gso_size;
		};
		break;
	}
	case GRANT: {
		struct grant_header *h = (struct grant_header *) header;
		char *resend = (h->resend_all) ? ", resend_all" : "";

		used = homa_snprintf(buffer, buf_len, used,
				", offset %d, grant_prio %u%s",
				ntohl(h->offset), h->priority, resend);
		break;
	}
	case RESEND: {
		struct resend_header *h = (struct resend_header *) header;

		used = homa_snprintf(buffer, buf_len, used,
				", offset %d, length %d, resend_prio %u",
				ntohl(h->offset), ntohl(h->length),
				h->priority);
		break;
	}
	case UNKNOWN:
		/* Nothing to add here. */
		break;
	case BUSY:
		/* Nothing to add here. */
		break;
	case CUTOFFS: {
		struct cutoffs_header *h = (struct cutoffs_header *) header;

		used = homa_snprintf(buffer, buf_len, used,
				", cutoffs %d %d %d %d %d %d %d %d, version %u",
				ntohl(h->unsched_cutoffs[0]),
				ntohl(h->unsched_cutoffs[1]),
				ntohl(h->unsched_cutoffs[2]),
				ntohl(h->unsched_cutoffs[3]),
				ntohl(h->unsched_cutoffs[4]),
				ntohl(h->unsched_cutoffs[5]),
				ntohl(h->unsched_cutoffs[6]),
				ntohl(h->unsched_cutoffs[7]),
				ntohs(h->cutoff_version));
		break;
	}
	case FREEZE:
		/* Nothing to add here. */
		break;
	case NEED_ACK:
		/* Nothing to add here. */
		break;
	case ACK: {
		struct ack_header *h = (struct ack_header *) header;
		int i, count;

		count = ntohs(h->num_acks);
		used = homa_snprintf(buffer, buf_len, used, ", acks");
		for (i = 0; i < count; i++) {
			used = homa_snprintf(buffer, buf_len, used,
					" [cp %d, sp %d, id %llu]",
					ntohs(h->acks[i].client_port),
					ntohs(h->acks[i].server_port),
					be64_to_cpu(h->acks[i].client_id));
		}
		break;
	}
	}

	buffer[buf_len-1] = 0;
	return buffer;
}

/**
 * homa_print_packet_short() - Print a human-readable string describing the
 * information in a Homa packet. This function generates a shorter
 * description than homa_print_packet.
 * @skb:     Packet whose information should be printed.
 * @buffer:  Buffer in which to generate the string.
 * @buf_len: Number of bytes available at @buffer.
 *
 * Return:   @buffer
 */
char *homa_print_packet_short(struct sk_buff *skb, char *buffer, int buf_len)
{
	char header[HOMA_MAX_HEADER];
	struct common_header *common = (struct common_header *) header;

	homa_skb_get(skb, header, 0, HOMA_MAX_HEADER);
	switch (common->type) {
	case DATA: {
		struct data_header *h = (struct data_header *)header;
		struct homa_skb_info *homa_info = homa_get_skb_info(skb);
		int data_left, used, i, seg_length, pos, offset;

		if (skb_shinfo(skb)->gso_segs == 0) {
			seg_length = homa_data_len(skb);
			data_left = 0;
		} else {
			seg_length = homa_info->seg_length;
			data_left = homa_info->data_bytes - seg_length;
		}
		offset = ntohl(h->seg.offset);
		if (offset == -1)
			offset = ntohl(h->common.sequence);

		pos = skb_transport_offset(skb) + sizeof32(*h) + seg_length;
		used = homa_snprintf(buffer, buf_len, 0, "DATA%s %d@%d",
				h->retransmit ? " retrans" : "",
				seg_length, offset);
		for (i = skb_shinfo(skb)->gso_segs - 1; i > 0; i--) {
			if (homa_info->seg_length < skb_shinfo(skb)->gso_size) {
				struct seg_header seg;

				homa_skb_get(skb, &seg, pos, sizeof(seg));
				offset = ntohl(seg.offset);
			} else {
				offset += seg_length;
			}
			if (seg_length > data_left)
				seg_length = data_left;
			used = homa_snprintf(buffer, buf_len, used,
					" %d@%d", seg_length, offset);
			data_left -= seg_length;
			pos += skb_shinfo(skb)->gso_size;
		}
		break;
	}
	case GRANT: {
		struct grant_header *h = (struct grant_header *) header;
		char *resend = h->resend_all ? " resend_all" : "";

		snprintf(buffer, buf_len, "GRANT %d@%d%s", ntohl(h->offset),
				h->priority, resend);
		break;
	}
	case RESEND: {
		struct resend_header *h = (struct resend_header *) header;

		snprintf(buffer, buf_len, "RESEND %d-%d@%d", ntohl(h->offset),
				ntohl(h->offset) + ntohl(h->length) - 1,
				h->priority);
		break;
	}
	case UNKNOWN:
		snprintf(buffer, buf_len, "UNKNOWN");
		break;
	case BUSY:
		snprintf(buffer, buf_len, "BUSY");
		break;
	case CUTOFFS:
		snprintf(buffer, buf_len, "CUTOFFS");
		break;
	case FREEZE:
		snprintf(buffer, buf_len, "FREEZE");
		break;
	case NEED_ACK:
		snprintf(buffer, buf_len, "NEED_ACK");
		break;
	case ACK:
		snprintf(buffer, buf_len, "ACK");
		break;
	default:
		snprintf(buffer, buf_len, "unknown packet type 0x%x",
				common->type);
		break;
	}
	return buffer;
}

/**
 * homa_freeze_peers() - Send FREEZE packets to all known peers.
 * @homa:   Provides info about peers.
 */
void homa_freeze_peers(struct homa *homa)
{
	struct homa_peer **peers;
	int num_peers, i, err;
	struct freeze_header freeze;
	struct homa_sock *hsk;
	struct homa_socktab_scan scan;

	/* Find a socket to use (any will do). */
	hsk = homa_socktab_start_scan(&homa->port_map, &scan);
	if (hsk == NULL) {
		tt_record("homa_freeze_peers couldn't find a socket");
		return;
	}

	peers = homa_peertab_get_peers(&homa->peers, &num_peers);
	if (peers == NULL) {
		tt_record("homa_freeze_peers couldn't find peers to freeze");
		return;
	}
	freeze.common.type = FREEZE;
	freeze.common.sport = htons(hsk->port);
	freeze.common.dport = 0;
	freeze.common.flags = HOMA_TCP_FLAGS;
	freeze.common.urgent = htons(HOMA_TCP_URGENT);
	freeze.common.sender_id = 0;
	for (i = 0; i < num_peers; i++) {
		tt_record1("Sending freeze to 0x%x", tt_addr(peers[i]->addr));
		err = __homa_xmit_control(&freeze, sizeof(freeze), peers[i], hsk);
		if (err != 0)
			tt_record2("homa_freeze_peers got error %d in xmit to 0x%x\n", err,
					tt_addr(peers[i]->addr));
	}
	kfree(peers);
}

/**
 * homa_snprintf() - This function makes it easy to use a series of calls
 * to snprintf to gradually append information to a fixed-size buffer.
 * If the buffer fills, the function can continue to be called, but nothing
 * more will get added to the buffer.
 * @buffer:   Characters accumulate here.
 * @size:     Total space available in @buffer.
 * @used:     Number of bytes currently occupied in the buffer, not including
 *            a terminating null character; this is typically the result of
 *            the previous call to this function.
 * @format:   Format string suitable for passing to printf-like functions,
 *            followed by values for the various substitutions requested
 *            in @format
 * @ ...
 *
 * Return:    The number of characters now occupied in @buffer, not
 *            including the terminating null character.
 */
int homa_snprintf(char *buffer, int size, int used, const char *format, ...)
{
	int new_chars;
	va_list ap;

	va_start(ap, format);

	if (used >= (size-1))
		return used;

	new_chars = vsnprintf(buffer + used, size - used, format, ap);
	if (new_chars < 0)
		return used;
	if (new_chars >= (size - used))
		return size - 1;
	return used + new_chars;
}

/**
 * homa_symbol_for_state() - Returns a printable string describing an
 * RPC state.
 * @rpc:  RPC whose state should be returned in printable form.
 *
 * Return: A static string holding the current state of @rpc.
 */
char *homa_symbol_for_state(struct homa_rpc *rpc)
{
	static char buffer[20];

	switch (rpc->state) {
	case RPC_OUTGOING:
		return "OUTGOING";
	case RPC_INCOMING:
		return "INCOMING";
	case RPC_IN_SERVICE:
		return "IN_SERVICE";
	case RPC_DEAD:
		return "DEAD";
	}

	/* See safety comment in homa_symbol_for_type. */
	snprintf(buffer, sizeof(buffer)-1, "unknown(%u)", rpc->state);
	buffer[sizeof(buffer)-1] = 0;
	return buffer;
}

/**
 * homa_symbol_for_type() - Returns a printable string describing a packet type.
 * @type:  A value from those defined by &homa_packet_type.
 *
 * Return: A static string holding the packet type corresponding to @type.
 */
char *homa_symbol_for_type(uint8_t type)
{
	static char buffer[20];

	switch (type) {
	case DATA:
		return "DATA";
	case GRANT:
		return "GRANT";
	case RESEND:
		return "RESEND";
	case UNKNOWN:
		return "UNKNOWN";
	case BUSY:
		return "BUSY";
	case CUTOFFS:
		return "CUTOFFS";
	case FREEZE:
		return "FREEZE";
	case NEED_ACK:
		return "NEED_ACK";
	case ACK:
		return "ACK";
	}

	/* Using a static buffer can produce garbled text under concurrency,
	 * but (a) it's unlikely (this code only executes if the opcode is
	 * bogus), (b) this is mostly for testing and debugging, and (c) the
	 * code below ensures that the string cannot run past the end of the
	 * buffer, so the code is safe.
	 */
	snprintf(buffer, sizeof(buffer)-1, "unknown(%u)", type);
	buffer[sizeof(buffer)-1] = 0;
	return buffer;
}

/**
 * homa_prios_changed() - This function is called whenever configuration
 * information related to priorities, such as @homa->unsched_cutoffs or
 * @homa->num_priorities, is modified. It adjusts the cutoffs if needed
 * to maintain consistency, and it updates other values that depend on
 * this information.
 * @homa: Contains the priority info to be checked and updated.
 */
void homa_prios_changed(struct homa *homa)
{
	int i;

	if (homa->num_priorities > HOMA_MAX_PRIORITIES)
		homa->num_priorities = HOMA_MAX_PRIORITIES;

	/* This guarantees that we will choose priority 0 if nothing else
	 * in the cutoff array matches.
	 */
	homa->unsched_cutoffs[0] = INT_MAX;

	for (i = HOMA_MAX_PRIORITIES-1; ; i--) {
		if (i >= homa->num_priorities) {
			homa->unsched_cutoffs[i] = 0;
			continue;
		}
		if (i == 0) {
			homa->unsched_cutoffs[i] = INT_MAX;
			homa->max_sched_prio = 0;
			break;
		}
		if (homa->unsched_cutoffs[i] >= HOMA_MAX_MESSAGE_LENGTH) {
			homa->max_sched_prio = i-1;
			break;
		}
	}
	homa->cutoff_version++;
}

/**
 * homa_spin() - Delay (without sleeping) for a given time interval.
 * @ns:   How long to delay (in nanoseconds)
 */
void homa_spin(int ns)
{
	__u64 end;

	end = get_cycles() + (ns*cpu_khz)/1000000;
	while (get_cycles() < end)
		/* Empty loop body.*/
		;
}

/**
 * homa_throttle_lock_slow() - This function implements the slow path for
 * acquiring the throttle lock. It is invoked when the lock isn't immediately
 * available. It waits for the lock, but also records statistics about
 * the waiting time.
 * @homa:    Overall data about the Homa protocol implementation.
 */
void homa_throttle_lock_slow(struct homa *homa)
{
	__u64 start = get_cycles();

	tt_record("beginning wait for throttle lock");
	spin_lock_bh(&homa->throttle_lock);
	tt_record("ending wait for throttle lock");
	INC_METRIC(throttle_lock_misses, 1);
	INC_METRIC(throttle_lock_miss_cycles, get_cycles() - start);
}

/**
 * homa_freeze() - Freezes the timetrace if a particular kind of freeze
 * has been requested through sysctl.
 * @rpc:      If we freeze our timetrace, we'll also send a freeze request
 *            to the peer for this RPC.
 * @type:     Condition that just occurred. If this doesn't match the
 *            externally set "freeze_type" value, then we don't freeze.
 * @format:   Format string used to generate a time trace record describing
 *            the reason for the freeze; must include "id %d, peer 0x%x"
 */
void homa_freeze(struct homa_rpc *rpc, enum homa_freeze_type type, char *format)
{
	if (type != rpc->hsk->homa->freeze_type)
		return;
	rpc->hsk->homa->freeze_type = 0;
	if (!tt_frozen) {
//		struct freeze_header freeze;
		int dummy;

		pr_notice("freezing in %s with freeze_type %d\n", __func__,
				type);
		tt_record1("homa_freeze calling homa_rpc_log_active with freeze_type %d", type);
		homa_rpc_log_active_tt(rpc->hsk->homa, 0);
		homa_validate_incoming(rpc->hsk->homa, 1, &dummy);
		pr_notice("%s\n", format);
		tt_record2(format, rpc->id, tt_addr(rpc->peer->addr));
		tt_freeze();
//		homa_xmit_control(FREEZE, &freeze, sizeof(freeze), rpc);
		homa_freeze_peers(rpc->hsk->homa);
	}
}
