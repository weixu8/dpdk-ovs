/*
 * Copyright 2012-2013 Intel Corporation All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Interface layer to communicate with Intel DPDK vSwitch. */

#include <rte_config.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>

#include "dpdk-link.h"
#include "dpif-dpdk.h"
#include "common.h"
#include <rte_log.h>

#include <assert.h>

#define PKT_BURST_SIZE 256

#ifdef PG_DEBUG
#define DPDK_DEBUG() printf("DPDK-LINK.c %s Line %d\n", __FUNCTION__, __LINE__);
#else
#define DPDK_DEBUG()
#endif

static struct rte_ring *rx_ring = NULL;
static struct rte_ring *tx_ring = NULL;
static struct rte_ring *packet_ring = NULL;
static struct rte_mempool *mp = NULL;

/* Sends 'packet' and 'request' data to datapath. */
int
dpdk_link_send(struct dpif_dpdk_message *request,
               const struct ofpbuf *const packet)
{
    return dpdk_link_send_bulk(request, &packet, 1);
}


/* Sends 'num_pkts' 'packets' and 'request' data to datapath. */
int
dpdk_link_send_bulk(struct dpif_dpdk_message *request,
                    const struct ofpbuf *const *packets, size_t num_pkts)
{
    struct rte_mbuf *mbufs[PKT_BURST_SIZE] = {NULL};
    uint8_t *mbuf_data = NULL;
    int i = 0;
    int ret = 0;

    if (num_pkts > PKT_BURST_SIZE) {
        return EINVAL;
    }

    DPDK_DEBUG()

    for (i = 0; i < num_pkts; i++) {
        mbufs[i] = rte_pktmbuf_alloc(mp);

        if (!mbufs[i]) {
            return ENOBUFS;
        }

        mbuf_data = rte_pktmbuf_mtod(mbufs[i], uint8_t *);
        rte_memcpy(mbuf_data, &request[i], sizeof(request[i]));

        if (request->type == DPIF_DPDK_PACKET_FAMILY) {
            mbuf_data = mbuf_data + sizeof(request[i]);
            if (likely(packets[i]->size <= (mbufs[i]->buf_len - sizeof(request[i])))) {
                rte_memcpy(mbuf_data, packets[i]->data, packets[i]->size);
                rte_pktmbuf_data_len(mbufs[i]) =
		                 sizeof(request[i]) + packets[i]->size;
                rte_pktmbuf_pkt_len(mbufs[i]) = rte_pktmbuf_data_len(mbufs[i]);
            } else {
                RTE_LOG(ERR, APP, "%s, %d: %s", __FUNCTION__, __LINE__,
                "memcpy prevented: packet size exceeds available mbuf space");
                for (i = 0; i < num_pkts; i++) {
                    rte_pktmbuf_free(mbufs[i]);
                }
                return ENOMEM;
            }
        } else {
            rte_pktmbuf_data_len(mbufs[i]) = sizeof(request[i]);
	    rte_pktmbuf_pkt_len(mbufs[i]) = rte_pktmbuf_data_len(mbufs[i]);
        }
    }

    ret = rte_ring_sp_enqueue_bulk(tx_ring, (void * const *)mbufs, num_pkts);
    if (ret == -ENOBUFS) {
        for (i = 0; i < num_pkts; i++) {
            rte_pktmbuf_free(mbufs[i]);
        }
        ret = ENOBUFS;
    } else if (unlikely(ret == -EDQUOT)) {
        ret = EDQUOT;
    }

    return ret;
}

/* Blocking function that waits for 'reply' from datapath. */
int
dpdk_link_recv_reply(struct dpif_dpdk_message *reply)
{
    struct rte_mbuf *mbuf = NULL;
    void *ctrlmbuf_data = NULL;
    int ctrlmbuf_len = 0;

    DPDK_DEBUG()

    while (rte_ring_sc_dequeue(rx_ring, (void **)&mbuf) != 0);

    ctrlmbuf_data = (void *)rte_ctrlmbuf_data(mbuf);
    ctrlmbuf_len = rte_ctrlmbuf_len(mbuf);
    rte_memcpy(reply, ctrlmbuf_data, ctrlmbuf_len);

    rte_ctrlmbuf_free(mbuf);

    return 0;
}

/* Blocking function that waits for a packet from datapath. 'pkt' will get
 * populated with packet data. */
int
dpdk_link_recv_packet(struct ofpbuf **pkt, struct dpif_dpdk_upcall *info)
{
    struct rte_mbuf *mbuf = NULL;
    uint16_t pktmbuf_len = 0;
    void *pktmbuf_data = NULL;

    DPDK_DEBUG()

    if (rte_ring_sc_dequeue(packet_ring, (void **)&mbuf) != 0) {
        return EAGAIN;
    }

    pktmbuf_data = rte_pktmbuf_mtod(mbuf, void *);
    pktmbuf_len = rte_pktmbuf_data_len(mbuf);
    rte_memcpy(info, pktmbuf_data, sizeof(*info));
    pktmbuf_data = (uint8_t *)pktmbuf_data + sizeof(*info);
    *pkt = ofpbuf_clone_data(pktmbuf_data, pktmbuf_len - sizeof(*info));

    rte_pktmbuf_free(mbuf);

    return 0;
}

/* Initialize DPDK link layer.
 *
 * No need to free any memory on shutdown as memory is owned by datapath.
 */
int
dpdk_link_init(void)
{
    DPDK_DEBUG()

    rx_ring = rte_ring_lookup(get_rx_queue_name(DATAPATH_RING_ID));
    if (rx_ring == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot get RX ring - is datapath running?\n");
    }

    tx_ring = rte_ring_lookup(get_tx_queue_name(DATAPATH_RING_ID));
    if (tx_ring == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot get TX ring - is datapath running?\n");
    }

    packet_ring = rte_ring_lookup(PACKET_RING_NAME);
    if (packet_ring == NULL) {
        rte_exit(EXIT_FAILURE,
                     "Cannot get packet RX ring - is datapath running?\n");
    }

    mp = rte_mempool_lookup(PKTMBUF_POOL_NAME);
    if (mp == NULL) {
        rte_exit(EXIT_FAILURE,
                      "Cannot get mempool for mbufs - is datapath running?\n");
    }

    return 0;
}
