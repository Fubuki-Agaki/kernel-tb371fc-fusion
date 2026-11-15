// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * vsock loopback transport.
 *
 * Backport of the upstream 5.9 loopback transport for this 4.19 tree, whose
 * vsock core already carries the 5.9-era virtio_transport_common pieces this
 * driver needs (struct virtio_transport wrapper, virtio_transport_recv_pkt()
 * taking the transport, packet-style send path).
 *
 * The TB371FC has no vsock bus transport (no VMCI, no virtio, no Hyper-V),
 * so without a transport the AF_VSOCK protocol family is never registered
 * (__vsock_core_init() is called by transports only) and socket(AF_VSOCK)
 * fails with EAFNOSUPPORT.  This loopback transport registers itself as the
 * transport and routes every packet back through the vsock core receive
 * path, so AF_VSOCK works entirely on-device: the traced service can bind()
 * its "vsock://-1:2010" listener and userspace clients can connect to any
 * CID (the bound-socket lookup matches by port, and each socket keeps the
 * CID the peer used to reach it).
 */

#include <linux/module.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/sock.h>
#include <net/af_vsock.h>
#include <linux/virtio_vsock.h>

/* Upstream introduced VMADDR_CID_LOCAL in the UAPI in 5.8; keep the define
 * local here to avoid touching the UAPI header. */
#define VMADDR_CID_LOCAL VMADDR_CID_RESERVED

struct vsock_loopback {
	struct workqueue_struct *wq;
	struct list_head pkts;
	spinlock_t pkt_lock;
	struct work_struct pkt_work;
};

static struct vsock_loopback the_vsock_loopback;

static struct virtio_transport loopback_transport;

static int vsock_loopback_send_pkt(struct virtio_vsock_pkt *pkt)
{
	struct vsock_loopback *the_loopback = &the_vsock_loopback;
	int len = pkt->len;

	spin_lock_bh(&the_loopback->pkt_lock);
	list_add_tail(&pkt->list, &the_loopback->pkts);
	spin_unlock_bh(&the_loopback->pkt_lock);

	queue_work(the_loopback->wq, &the_loopback->pkt_work);

	return len;
}

static void vsock_loopback_pkt_work(struct work_struct *work)
{
	struct vsock_loopback *the_loopback =
		container_of(work, struct vsock_loopback, pkt_work);
	struct virtio_vsock_pkt *pkt;
	LIST_HEAD(pkts);

	spin_lock_bh(&the_loopback->pkt_lock);
	list_splice_init(&the_loopback->pkts, &pkts);
	spin_unlock_bh(&the_loopback->pkt_lock);

	while ((pkt = list_first_entry_or_null(&pkts,
					       struct virtio_vsock_pkt,
					       list)) != NULL) {
		list_del_init(&pkt->list);
		virtio_transport_recv_pkt(&loopback_transport, pkt);
	}
}

static int vsock_loopback_cancel_pkt(struct vsock_sock *vsk)
{
	struct vsock_loopback *the_loopback = &the_vsock_loopback;
	struct virtio_vsock_pkt *pkt, *pkt_tmp;
	LIST_HEAD(freelist);

	spin_lock_bh(&the_loopback->pkt_lock);

	list_for_each_entry_safe(pkt, pkt_tmp, &the_loopback->pkts, list) {
		if (pkt->vsk != vsk)
			continue;

		list_move_tail(&pkt->list, &freelist);
	}

	spin_unlock_bh(&the_loopback->pkt_lock);

	list_for_each_entry_safe(pkt, pkt_tmp, &freelist, list) {
		if (pkt->reply)
			virtio_transport_recv_pkt(&loopback_transport, pkt);
		else
			virtio_transport_free_pkt(pkt);
	}

	return 0;
}

static u32 vsock_loopback_get_local_cid(void)
{
	return VMADDR_CID_LOCAL;
}

static struct virtio_transport loopback_transport = {
	.transport = {
		.get_local_cid            = vsock_loopback_get_local_cid,

		.init                     = virtio_transport_do_socket_init,
		.destruct                 = virtio_transport_destruct,
		.release                  = virtio_transport_release,
		.connect                  = virtio_transport_connect,
		.shutdown                 = virtio_transport_shutdown,
		.cancel_pkt               = vsock_loopback_cancel_pkt,

		.dgram_bind               = virtio_transport_dgram_bind,
		.dgram_dequeue            = virtio_transport_dgram_dequeue,
		.dgram_enqueue            = virtio_transport_dgram_enqueue,
		.dgram_allow              = virtio_transport_dgram_allow,

		.stream_dequeue           = virtio_transport_stream_dequeue,
		.stream_enqueue           = virtio_transport_stream_enqueue,
		.stream_has_data          = virtio_transport_stream_has_data,
		.stream_has_space         = virtio_transport_stream_has_space,
		.stream_rcvhiwat          = virtio_transport_stream_rcvhiwat,
		.stream_is_active         = virtio_transport_stream_is_active,
		.stream_allow             = virtio_transport_stream_allow,

		.notify_poll_in           = virtio_transport_notify_poll_in,
		.notify_poll_out          = virtio_transport_notify_poll_out,
		.notify_recv_init         = virtio_transport_notify_recv_init,
		.notify_recv_pre_block    = virtio_transport_notify_recv_pre_block,
		.notify_recv_pre_dequeue  = virtio_transport_notify_recv_pre_dequeue,
		.notify_recv_post_dequeue = virtio_transport_notify_recv_post_dequeue,
		.notify_send_init         = virtio_transport_notify_send_init,
		.notify_send_pre_block    = virtio_transport_notify_send_pre_block,
		.notify_send_pre_enqueue  = virtio_transport_notify_send_pre_enqueue,
		.notify_send_post_enqueue = virtio_transport_notify_send_post_enqueue,

		.set_buffer_size          = virtio_transport_set_buffer_size,
		.set_min_buffer_size      = virtio_transport_set_min_buffer_size,
		.set_max_buffer_size      = virtio_transport_set_max_buffer_size,
		.get_buffer_size          = virtio_transport_get_buffer_size,
		.get_min_buffer_size      = virtio_transport_get_min_buffer_size,
		.get_max_buffer_size      = virtio_transport_get_max_buffer_size,
	},

	.send_pkt = vsock_loopback_send_pkt,
};

static int __init vsock_loopback_init(void)
{
	struct vsock_loopback *the_loopback = &the_vsock_loopback;
	int ret;

	the_loopback->wq = alloc_workqueue("vsock-loopback", 0, 0);
	if (!the_loopback->wq)
		return -ENOMEM;

	spin_lock_init(&the_loopback->pkt_lock);
	INIT_LIST_HEAD(&the_loopback->pkts);
	INIT_WORK(&the_loopback->pkt_work, vsock_loopback_pkt_work);

	ret = __vsock_core_init(&loopback_transport.transport, THIS_MODULE);
	if (ret)
		goto out_wq;

	return 0;

out_wq:
	destroy_workqueue(the_loopback->wq);
	return ret;
}

static void __exit vsock_loopback_exit(void)
{
	struct vsock_loopback *the_loopback = &the_vsock_loopback;
	struct virtio_vsock_pkt *pkt, *pkt_tmp;

	vsock_core_exit();

	cancel_work_sync(&the_loopback->pkt_work);
	destroy_workqueue(the_loopback->wq);

	list_for_each_entry_safe(pkt, pkt_tmp, &the_loopback->pkts, list) {
		list_del_init(&pkt->list);
		virtio_transport_free_pkt(pkt);
	}
}

module_init(vsock_loopback_init);
module_exit(vsock_loopback_exit);
MODULE_AUTHOR("Stefano Garzarella <sgarzare@redhat.com>");
MODULE_DESCRIPTION("vsock loopback transport");
MODULE_LICENSE("GPL");
