/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 */

#ifndef __NEURON_H__
#define __NEURON_H__

#include <linux/device.h>
#include <linux/skbuff.h>
#include <linux/rcupdate.h>

/*
 * Communication channels
 *
 * These are link-layer devices which abstract the details of inter-VM
 * communication mechanisms away from the upper layers.
 */

enum neuron_channel_type {
	NEURON_CHANNEL_MESSAGE_QUEUE = 1,
	NEURON_CHANNEL_NOTIFICATION,
	NEURON_CHANNEL_SHARED_MEMORY
};

enum neuron_channel_direction {
	NEURON_CHANNEL_SEND = (1 << 0),
	NEURON_CHANNEL_RECEIVE = (1 << 1),
	NEURON_CHANNEL_BIDIRECTIONAL = NEURON_CHANNEL_SEND |
		NEURON_CHANNEL_RECEIVE
};

struct buffer_list {
	struct sk_buff *head;
	off_t offset;
	size_t size;
};

struct neuron_channel {
	enum neuron_channel_type type;
	enum neuron_channel_direction direction;

	/* For message queue channels, the maximum guaranteed message size
	 * and the minimum guaranteed message queue length. These may be
	 * zero until handshaking with the peer has completed; in this case,
	 * the channel driver will call the wakeup callback after they have
	 * been set.
	 *
	 * Note that it may be transiently possible to exceed these limits;
	 * they are merely the lower bounds guaranteed by the driver.
	 */
	size_t max_size;
	unsigned int queue_length;

	struct device dev;
	struct neuron_protocol *protocol;
	unsigned int id;

	/* Writes protected by the protocol device lock */
	struct neuron_protocol_driver __rcu *protocol_drv;
};

#define to_neuron_channel(drv) container_of(drv, struct neuron_channel, dev)

struct neuron_channel_driver {
	enum neuron_channel_type type;
	enum neuron_channel_direction direction;
	struct device_driver driver;

	int (*probe)(struct neuron_channel *channel_dev);
	void (*remove)(struct neuron_channel *channel_dev);

	/* Message queue send callback.
	 * @skb sk_buff pointer for sending.
	 * @return 0 for success, others for failure.
	 */
	int (*send_msg)(struct neuron_channel *channel_dev,
			struct sk_buff *skb);
	/* Message queue send callback.
	 * @buf buffer_list object, which contains the offset of the sk buffer
	 * and size to send.
	 * @return 0 for success, others for failure.
	 */
	int (*send_msgv)(struct neuron_channel *channel_dev,
			 struct buffer_list buf);
	/* Message queue receive callbacks
	 * The caller is responsible for allocating and freeing receiving buffer
	 * @skb sk_buff pointer for receiving.
	 * @return positive number for received data length, negative number for
	 * failure. Never return 0.
	 */
	ssize_t (*receive_msg)(struct neuron_channel *channel_dev,
			       struct sk_buff *skb);
	/* Message queue send callback.
	 * @buf buffer_list object, which contains the offset of the sk buffer
	 * to start taking the received data.
	 * @return positive number for received data length, negative number for
	 * failure. Never return 0.
	 */
	ssize_t (*receive_msgv)(struct neuron_channel *channel_dev,
				struct buffer_list buf);

	/* Notification callbacks */
	int (*send_notify)(struct neuron_channel *channel_dev, uint32_t bits);
	uint32_t (*receive_notify)(struct neuron_channel *channel_dev);
};

#define to_neuron_channel_driver(drv) container_of(drv, \
		struct neuron_channel_driver, driver)

struct neuron_channel *neuron_channel_add(struct device_node *node,
					struct device *parent);
int neuron_register_channel_driver(struct neuron_channel_driver *drv);
void neuron_unregister_channel_driver(struct neuron_channel_driver *drv);


/*
 * Protocol drivers (typically autogenerated)
 *
 * These drivers translate between messages that are sent over the
 * communication channels and high-level interfaces that are used by the
 * application layers.
 *
 * Each driver has its own set of callbacks that communicate with compatible
 * application drivers, and expects a particular set of channel devices.
 * These will typically be defined by enclosing struct neuron_protocol_driver
 * in a protocol-specific structure which the application driver accesses.
 */

struct neuron_protocol {
	struct device dev;
	struct neuron_application *application;

	unsigned int process_count;
	const char **processes;

	unsigned int channel_count;
	struct neuron_channel *channels[];
};

#define to_neuron_protocol(drv) container_of(drv, struct neuron_protocol, dev)

struct neuron_channel_match_table {
	enum neuron_channel_type type;
	enum neuron_channel_direction direction;
};

struct neuron_protocol_driver {
	unsigned int channel_count;
	const struct neuron_channel_match_table *channels;

	unsigned int process_count;
	const char *const *processes;

	struct device_driver driver;

	int (*probe)(struct neuron_protocol *protocol_dev);
	void (*remove)(struct neuron_protocol *protocol_dev);

	int (*channel_wakeup)(struct neuron_protocol *protocol,
			      unsigned int id);
	int (*app_wakeup)(struct neuron_protocol *dev, unsigned int ev);
};

#define to_neuron_protocol_driver(drv) container_of(drv, \
		struct neuron_protocol_driver, driver)

struct neuron_protocol *neuron_protocol_add(struct device_node *node,
		unsigned int channel_count, struct neuron_channel **channels,
		struct device *parent, struct neuron_application *app_dev);
int neuron_register_protocol_driver(struct neuron_protocol_driver *drv);
void neuron_unregister_protocol_driver(struct neuron_protocol_driver *drv);

/**
 * neuron_channel_wakeup() - tell a protocol that a channel is ready
 *
 * This function should be called by the channel driver when its channel first
 * becomes fully initialised, and also when the channel becomes ready to send
 * or receive data. It will call a method provided by the protocol driver
 * which will typically wake up a wait queue or schedule a tasklet to process
 * the data. The wakeup method will not block.
 *
 * For message queue channels, this is triggered:
 * - after the channel's maximum message size and queue length are known and
 *   handshaking with the peer has completed;
 * - when a send side channel that was previously full is no longer full; and
 * - when a receive side channel that was previously empty is no longer empty.
 *
 * For notification channels, this is triggered when a receive side channel
 * may have received a notification from its remote partner. It is not used on
 * send side notification channels.
 *
 * This is unused for shared-memory channels.
 */
static inline int neuron_channel_wakeup(struct neuron_channel *channel)
{
	struct neuron_protocol_driver *protocol_drv;
	int ret = -ECONNRESET;

	rcu_read_lock();

	protocol_drv = rcu_dereference(channel->protocol_drv);
	if (protocol_drv != NULL)
		if (protocol_drv->channel_wakeup)
			ret = protocol_drv->channel_wakeup(channel->protocol,
							   channel->id);

	rcu_read_unlock();

	return ret;
}

/*
 * Application drivers
 *
 * These drivers contain hand-written glue between the high-level API provided
 * by a protocol driver, and the guest kernel's internal interfaces.
 */

struct neuron_application {
	const char *type;
	struct device dev;
	struct neuron_protocol *protocol;

	/* Writes protected by the protocol device lock */
	struct neuron_protocol_driver __rcu *protocol_drv;
};

#define to_neuron_application(drv) container_of(drv, \
		struct neuron_application, dev)

struct neuron_app_driver {
	struct device_driver driver;

	const struct neuron_protocol_driver *protocol_driver;

	int (*probe)(struct neuron_application *dev);
	void (*remove)(struct neuron_application *dev);
	void (*start)(struct neuron_application *dev);
};

#define to_neuron_app_driver(drv) container_of(drv, \
		struct neuron_app_driver, driver)

struct neuron_application *neuron_app_add(struct device_node *node,
		struct device *parent);
int neuron_register_app_driver(struct neuron_app_driver *drv);
void neuron_unregister_app_driver(struct neuron_app_driver *drv);

/**
 * neuron_app_wakeup() - tell a protocol that the application is ready
 *
 * This function should be called by the application driver when there is a
 * wakeup that needs to be sent to the protocol driver.
 *
 */
static inline int neuron_app_wakeup(struct neuron_application *application,
				    unsigned int ev)
{
	struct neuron_protocol_driver *protocol_drv;
	int ret = -ECONNRESET;

	rcu_read_lock();

	protocol_drv = rcu_dereference(application->protocol_drv);
	if (protocol_drv != NULL)
		if (protocol_drv->app_wakeup)
			ret = protocol_drv->app_wakeup(application->protocol,
									ev);

	rcu_read_unlock();

	return ret;
}

/**
 * Allocate sk_buff with pages as many as you want.
 */
static inline struct sk_buff *neuron_alloc_pskb(size_t data_len, gfp_t gfp)
{
	struct sk_buff *head_skb = NULL;
	struct sk_buff *second_skb = NULL;
	struct sk_buff *new_frag, *prev;
	int ret;

	do {
		size_t frag_len = min_t(size_t, data_len,
					MAX_SKB_FRAGS << PAGE_SHIFT);

		new_frag = alloc_skb_with_frags(0, frag_len,
				0, &ret, gfp);
		if (!new_frag) {
			if (head_skb)
				kfree_skb(head_skb);
			return ERR_PTR(ret);
		}
		new_frag->data_len = frag_len;
		new_frag->len = frag_len;

		if (!head_skb) {
			head_skb = new_frag;
		} else {
			if (!second_skb) {
				skb_shinfo(head_skb)->frag_list = new_frag;
				second_skb = new_frag;
			} else {
				prev->next = new_frag;
			}
			prev = new_frag;

			head_skb->len += new_frag->len;
			head_skb->data_len += new_frag->data_len;
			head_skb->truesize += new_frag->truesize;
		}

		data_len -= frag_len;
	} while (data_len);

	return head_skb;
}

#endif /* __LINUX_NEURON_H */
