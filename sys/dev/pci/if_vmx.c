/*	$NetBSD: if_vmx.c,v 1.7 2022/03/30 02:45:14 knakahara Exp $	*/
/*	$OpenBSD: if_vmx.c,v 1.16 2014/01/22 06:04:17 brad Exp $	*/

/*
 * Copyright (c) 2013 Tsubai Masanari
 * Copyright (c) 2013 Bryan Venteicher <bryanv@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: if_vmx.c,v 1.7 2022/03/30 02:45:14 knakahara Exp $");

#include <sys/param.h>
#include <sys/cpu.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/bitops.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/sockio.h>
#include <sys/pcq.h>
#include <sys/workqueue.h>
#include <sys/interrupt.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>

#include <netinet/if_inarp.h>
#include <netinet/in_systm.h>	/* for <netinet/ip.h> */
#include <netinet/in.h>		/* for <netinet/ip.h> */
#include <netinet/ip.h>		/* for struct ip */
#include <netinet/ip6.h>	/* for struct ip6_hdr */
#include <netinet/tcp.h>	/* for struct tcphdr */
#include <netinet/udp.h>	/* for struct udphdr */

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>

#include <dev/pci/if_vmxreg.h>

#define VMXNET3_DRIVER_VERSION 0x00010000

/*
 * Max descriptors per Tx packet. We must limit the size of the
 * any TSO packets based on the number of segments.
 */
#define VMXNET3_TX_MAXSEGS		32
#define VMXNET3_TX_MAXSIZE		(VMXNET3_TX_MAXSEGS * MCLBYTES)

/*
 * Maximum support Tx segments size. The length field in the
 * Tx descriptor is 14 bits.
 */
#define VMXNET3_TX_MAXSEGSIZE		(1 << 14)

/*
 * The maximum number of Rx segments we accept.
 */
#define VMXNET3_MAX_RX_SEGS		0	/* no segments */

/*
 * Predetermined size of the multicast MACs filter table. If the
 * number of multicast addresses exceeds this size, then the
 * ALL_MULTI mode is use instead.
 */
#define VMXNET3_MULTICAST_MAX		32

/*
 * Our Tx watchdog timeout.
 */
#define VMXNET3_WATCHDOG_TIMEOUT	5

/*
 * Default value for vmx_intr_{rx,tx}_process_limit which is used for
 * max number of packets to process for interrupt handler
 */
#define VMXNET3_RX_INTR_PROCESS_LIMIT 0U
#define VMXNET3_TX_INTR_PROCESS_LIMIT 256

/*
 * Default value for vmx_{rx,tx}_process_limit which is used for
 * max number of packets to process for deferred processing
 */
#define VMXNET3_RX_PROCESS_LIMIT 256
#define VMXNET3_TX_PROCESS_LIMIT 256

#define VMXNET3_WORKQUEUE_PRI PRI_SOFTNET

/*
 * IP protocols that we can perform Tx checksum offloading of.
 */
#define VMXNET3_CSUM_OFFLOAD \
    (M_CSUM_TCPv4 | M_CSUM_UDPv4)
#define VMXNET3_CSUM_OFFLOAD_IPV6 \
    (M_CSUM_TCPv6 | M_CSUM_UDPv6)

#define VMXNET3_CSUM_ALL_OFFLOAD \
    (VMXNET3_CSUM_OFFLOAD | VMXNET3_CSUM_OFFLOAD_IPV6 | M_CSUM_TSOv4 | M_CSUM_TSOv6)

#define VMXNET3_RXRINGS_PERQ 2

#define VMXNET3_CORE_LOCK(_sc)		mutex_enter((_sc)->vmx_mtx)
#define VMXNET3_CORE_UNLOCK(_sc)	mutex_exit((_sc)->vmx_mtx)
#define VMXNET3_CORE_LOCK_ASSERT(_sc)	mutex_owned((_sc)->vmx_mtx)

#define VMXNET3_RXQ_LOCK(_rxq)		mutex_enter((_rxq)->vxrxq_mtx)
#define VMXNET3_RXQ_UNLOCK(_rxq)	mutex_exit((_rxq)->vxrxq_mtx)
#define VMXNET3_RXQ_LOCK_ASSERT(_rxq)		\
    mutex_owned((_rxq)->vxrxq_mtx)

#define VMXNET3_TXQ_LOCK(_txq)		mutex_enter((_txq)->vxtxq_mtx)
#define VMXNET3_TXQ_TRYLOCK(_txq)	mutex_tryenter((_txq)->vxtxq_mtx)
#define VMXNET3_TXQ_UNLOCK(_txq)	mutex_exit((_txq)->vxtxq_mtx)
#define VMXNET3_TXQ_LOCK_ASSERT(_txq)		\
    mutex_owned((_txq)->vxtxq_mtx)

struct vmxnet3_dma_alloc {
	bus_addr_t dma_paddr;
	void *dma_vaddr;
	bus_dmamap_t dma_map;
	bus_size_t dma_size;
	bus_dma_segment_t dma_segs[1];
};

struct vmxnet3_txbuf {
	bus_dmamap_t vtxb_dmamap;
	struct mbuf *vtxb_m;
};

struct vmxnet3_txring {
	struct vmxnet3_txbuf *vxtxr_txbuf;
	struct vmxnet3_txdesc *vxtxr_txd;
	u_int vxtxr_head;
	u_int vxtxr_next;
	u_int vxtxr_ndesc;
	int vxtxr_gen;
	struct vmxnet3_dma_alloc vxtxr_dma;
};

struct vmxnet3_rxbuf {
	bus_dmamap_t vrxb_dmamap;
	struct mbuf *vrxb_m;
};

struct vmxnet3_rxring {
	struct vmxnet3_rxbuf *vxrxr_rxbuf;
	struct vmxnet3_rxdesc *vxrxr_rxd;
	u_int vxrxr_fill;
	u_int vxrxr_ndesc;
	int vxrxr_gen;
	int vxrxr_rid;
	struct vmxnet3_dma_alloc vxrxr_dma;
	bus_dmamap_t vxrxr_spare_dmap;
};

struct vmxnet3_comp_ring {
	union {
		struct vmxnet3_txcompdesc *txcd;
		struct vmxnet3_rxcompdesc *rxcd;
	} vxcr_u;
	u_int vxcr_next;
	u_int vxcr_ndesc;
	int vxcr_gen;
	struct vmxnet3_dma_alloc vxcr_dma;
};

struct vmxnet3_txq_stats {
#if 0
	uint64_t vmtxs_opackets;	/* if_opackets */
	uint64_t vmtxs_obytes;		/* if_obytes */
	uint64_t vmtxs_omcasts;		/* if_omcasts */
#endif
	uint64_t vmtxs_csum;
	uint64_t vmtxs_tso;
	uint64_t vmtxs_full;
	uint64_t vmtxs_offload_failed;
};

struct vmxnet3_txqueue {
	kmutex_t *vxtxq_mtx;
	struct vmxnet3_softc *vxtxq_sc;
	int vxtxq_watchdog;
	pcq_t *vxtxq_interq;
	struct vmxnet3_txring vxtxq_cmd_ring;
	struct vmxnet3_comp_ring vxtxq_comp_ring;
	struct vmxnet3_txq_stats vxtxq_stats;
	struct vmxnet3_txq_shared *vxtxq_ts;
	char vxtxq_name[16];

	void *vxtxq_si;

	struct evcnt vxtxq_intr;
	struct evcnt vxtxq_defer;
	struct evcnt vxtxq_deferreq;
	struct evcnt vxtxq_pcqdrop;
	struct evcnt vxtxq_transmitdef;
	struct evcnt vxtxq_watchdogto;
	struct evcnt vxtxq_defragged;
	struct evcnt vxtxq_defrag_failed;
};

#if 0
struct vmxnet3_rxq_stats {
	uint64_t vmrxs_ipackets;	/* if_ipackets */
	uint64_t vmrxs_ibytes;		/* if_ibytes */
	uint64_t vmrxs_iqdrops;		/* if_iqdrops */
	uint64_t vmrxs_ierrors;		/* if_ierrors */
};
#endif

struct vmxnet3_rxqueue {
	kmutex_t *vxrxq_mtx;
	struct vmxnet3_softc *vxrxq_sc;
	struct mbuf *vxrxq_mhead;
	struct mbuf *vxrxq_mtail;
	struct vmxnet3_rxring vxrxq_cmd_ring[VMXNET3_RXRINGS_PERQ];
	struct vmxnet3_comp_ring vxrxq_comp_ring;
#if 0
	struct vmxnet3_rxq_stats vxrxq_stats;
#endif
	struct vmxnet3_rxq_shared *vxrxq_rs;
	char vxrxq_name[16];

	struct evcnt vxrxq_intr;
	struct evcnt vxrxq_defer;
	struct evcnt vxrxq_deferreq;
	struct evcnt vxrxq_mgetcl_failed;
	struct evcnt vxrxq_mbuf_load_failed;
};

struct vmxnet3_queue {
	int vxq_id;
	int vxq_intr_idx;

	struct vmxnet3_txqueue vxq_txqueue;
	struct vmxnet3_rxqueue vxq_rxqueue;

	void *vxq_si;
	bool vxq_workqueue;
	bool vxq_wq_enqueued;
	struct work vxq_wq_cookie;
};

struct vmxnet3_softc {
	device_t vmx_dev;
	struct ethercom vmx_ethercom;
	struct ifmedia vmx_media;
	struct vmxnet3_driver_shared *vmx_ds;
	int vmx_flags;
#define VMXNET3_FLAG_NO_MSIX	(1 << 0)
#define VMXNET3_FLAG_RSS	(1 << 1)
#define VMXNET3_FLAG_ATTACHED	(1 << 2)

	struct vmxnet3_queue *vmx_queue;

	struct pci_attach_args *vmx_pa;
	pci_chipset_tag_t vmx_pc;

	bus_space_tag_t vmx_iot0;
	bus_space_tag_t vmx_iot1;
	bus_space_handle_t vmx_ioh0;
	bus_space_handle_t vmx_ioh1;
	bus_size_t vmx_ios0;
	bus_size_t vmx_ios1;
	bus_dma_tag_t vmx_dmat;

	int vmx_link_active;
	int vmx_ntxqueues;
	int vmx_nrxqueues;
	int vmx_ntxdescs;
	int vmx_nrxdescs;
	int vmx_max_rxsegs;

	struct evcnt vmx_event_intr;
	struct evcnt vmx_event_link;
	struct evcnt vmx_event_txqerror;
	struct evcnt vmx_event_rxqerror;
	struct evcnt vmx_event_dic;
	struct evcnt vmx_event_debug;

	int vmx_intr_type;
	int vmx_intr_mask_mode;
	int vmx_event_intr_idx;
	int vmx_nintrs;
	pci_intr_handle_t *vmx_intrs;	/* legacy use vmx_intrs[0] */
	void *vmx_ihs[VMXNET3_MAX_INTRS];

	kmutex_t *vmx_mtx;

	uint8_t *vmx_mcast;
	void *vmx_qs;
	struct vmxnet3_rss_shared *vmx_rss;
	callout_t vmx_tick;
	struct vmxnet3_dma_alloc vmx_ds_dma;
	struct vmxnet3_dma_alloc vmx_qs_dma;
	struct vmxnet3_dma_alloc vmx_mcast_dma;
	struct vmxnet3_dma_alloc vmx_rss_dma;
	int vmx_max_ntxqueues;
	int vmx_max_nrxqueues;
	uint8_t vmx_lladdr[ETHER_ADDR_LEN];

	u_int vmx_rx_intr_process_limit;
	u_int vmx_tx_intr_process_limit;
	u_int vmx_rx_process_limit;
	u_int vmx_tx_process_limit;
	struct sysctllog *vmx_sysctllog;

	bool vmx_txrx_workqueue;
	struct workqueue *vmx_queue_wq;
};

#define VMXNET3_STAT

#ifdef VMXNET3_STAT
struct {
	u_int txhead;
	u_int txdone;
	u_int maxtxlen;
	u_int rxdone;
	u_int rxfill;
	u_int intr;
} vmxstat;
#endif

typedef enum {
	VMXNET3_BARRIER_RD,
	VMXNET3_BARRIER_WR,
} vmxnet3_barrier_t;

#define JUMBO_LEN (MCLBYTES - ETHER_ALIGN)	/* XXX */
#define DMAADDR(map) ((map)->dm_segs[0].ds_addr)

#define vtophys(va) 0		/* XXX ok? */

static int vmxnet3_match(device_t, cfdata_t, void *);
static void vmxnet3_attach(device_t, device_t, void *);
static int vmxnet3_detach(device_t, int);

static int vmxnet3_alloc_pci_resources(struct vmxnet3_softc *);
static void vmxnet3_free_pci_resources(struct vmxnet3_softc *);
static int vmxnet3_check_version(struct vmxnet3_softc *);
static void vmxnet3_check_multiqueue(struct vmxnet3_softc *);

static int vmxnet3_alloc_msix_interrupts(struct vmxnet3_softc *);
static int vmxnet3_alloc_msi_interrupts(struct vmxnet3_softc *);
static int vmxnet3_alloc_legacy_interrupts(struct vmxnet3_softc *);
static int vmxnet3_alloc_interrupts(struct vmxnet3_softc *);
static void vmxnet3_free_interrupts(struct vmxnet3_softc *);

static int vmxnet3_setup_msix_interrupts(struct vmxnet3_softc *);
static int vmxnet3_setup_msi_interrupt(struct vmxnet3_softc *);
static int vmxnet3_setup_legacy_interrupt(struct vmxnet3_softc *);
static void vmxnet3_set_interrupt_idx(struct vmxnet3_softc *);
static int vmxnet3_setup_interrupts(struct vmxnet3_softc *);
static int vmxnet3_setup_sysctl(struct vmxnet3_softc *);

static int vmxnet3_setup_stats(struct vmxnet3_softc *);
static void vmxnet3_teardown_stats(struct vmxnet3_softc *);

static int vmxnet3_init_rxq(struct vmxnet3_softc *, int);
static int vmxnet3_init_txq(struct vmxnet3_softc *, int);
static int vmxnet3_alloc_rxtx_queues(struct vmxnet3_softc *);
static void vmxnet3_destroy_rxq(struct vmxnet3_rxqueue *);
static void vmxnet3_destroy_txq(struct vmxnet3_txqueue *);
static void vmxnet3_free_rxtx_queues(struct vmxnet3_softc *);

static int vmxnet3_alloc_shared_data(struct vmxnet3_softc *);
static void vmxnet3_free_shared_data(struct vmxnet3_softc *);
static int vmxnet3_alloc_txq_data(struct vmxnet3_softc *);
static void vmxnet3_free_txq_data(struct vmxnet3_softc *);
static int vmxnet3_alloc_rxq_data(struct vmxnet3_softc *);
static void vmxnet3_free_rxq_data(struct vmxnet3_softc *);
static int vmxnet3_alloc_queue_data(struct vmxnet3_softc *);
static void vmxnet3_free_queue_data(struct vmxnet3_softc *);
static int vmxnet3_alloc_mcast_table(struct vmxnet3_softc *);
static void vmxnet3_free_mcast_table(struct vmxnet3_softc *);
static void vmxnet3_init_shared_data(struct vmxnet3_softc *);
static void vmxnet3_reinit_rss_shared_data(struct vmxnet3_softc *);
static void vmxnet3_reinit_shared_data(struct vmxnet3_softc *);
static int vmxnet3_alloc_data(struct vmxnet3_softc *);
static void vmxnet3_free_data(struct vmxnet3_softc *);
static int vmxnet3_setup_interface(struct vmxnet3_softc *);

static void vmxnet3_evintr(struct vmxnet3_softc *);
static bool vmxnet3_txq_eof(struct vmxnet3_txqueue *, u_int);
static int vmxnet3_newbuf(struct vmxnet3_softc *, struct vmxnet3_rxqueue *,
    struct vmxnet3_rxring *);
static void vmxnet3_rxq_eof_discard(struct vmxnet3_rxqueue *,
    struct vmxnet3_rxring *, int);
static void vmxnet3_rxq_discard_chain(struct vmxnet3_rxqueue *);
static void vmxnet3_rx_csum(struct vmxnet3_rxcompdesc *, struct mbuf *);
static void vmxnet3_rxq_input(struct vmxnet3_rxqueue *,
    struct vmxnet3_rxcompdesc *, struct mbuf *);
static bool vmxnet3_rxq_eof(struct vmxnet3_rxqueue *, u_int);
static int vmxnet3_legacy_intr(void *);
static int vmxnet3_txrxq_intr(void *);
static void vmxnet3_handle_queue(void *);
static void vmxnet3_handle_queue_work(struct work *, void *);
static int vmxnet3_event_intr(void *);

static void vmxnet3_txstop(struct vmxnet3_softc *, struct vmxnet3_txqueue *);
static void vmxnet3_rxstop(struct vmxnet3_softc *, struct vmxnet3_rxqueue *);
static void vmxnet3_stop_locked(struct vmxnet3_softc *);
static void vmxnet3_stop_rendezvous(struct vmxnet3_softc *);
static void vmxnet3_stop(struct ifnet *, int);

static void vmxnet3_txinit(struct vmxnet3_softc *, struct vmxnet3_txqueue *);
static int vmxnet3_rxinit(struct vmxnet3_softc *, struct vmxnet3_rxqueue *);
static int vmxnet3_reinit_queues(struct vmxnet3_softc *);
static int vmxnet3_enable_device(struct vmxnet3_softc *);
static void vmxnet3_reinit_rxfilters(struct vmxnet3_softc *);
static int vmxnet3_reinit(struct vmxnet3_softc *);

static int vmxnet3_init_locked(struct vmxnet3_softc *);
static int vmxnet3_init(struct ifnet *);

static int vmxnet3_txq_offload_ctx(struct vmxnet3_txqueue *, struct mbuf *, int *, int *);
static int vmxnet3_txq_load_mbuf(struct vmxnet3_txqueue *, struct mbuf **, bus_dmamap_t);
static void vmxnet3_txq_unload_mbuf(struct vmxnet3_txqueue *, bus_dmamap_t);
static int vmxnet3_txq_encap(struct vmxnet3_txqueue *, struct mbuf **);
static void vmxnet3_start_locked(struct ifnet *);
static void vmxnet3_start(struct ifnet *);
static void vmxnet3_transmit_locked(struct ifnet *, struct vmxnet3_txqueue *);
static int vmxnet3_transmit(struct ifnet *, struct mbuf *);
static void vmxnet3_deferred_transmit(void *);

static void vmxnet3_set_rxfilter(struct vmxnet3_softc *);
static int vmxnet3_ioctl(struct ifnet *, u_long, void *);
static int vmxnet3_ifflags_cb(struct ethercom *);

static int vmxnet3_watchdog(struct vmxnet3_txqueue *);
static void vmxnet3_refresh_host_stats(struct vmxnet3_softc *);
static void vmxnet3_tick(void *);
static void vmxnet3_if_link_status(struct vmxnet3_softc *);
static bool vmxnet3_cmd_link_status(struct ifnet *);
static void vmxnet3_ifmedia_status(struct ifnet *, struct ifmediareq *);
static int vmxnet3_ifmedia_change(struct ifnet *);
static void vmxnet3_set_lladdr(struct vmxnet3_softc *);
static void vmxnet3_get_lladdr(struct vmxnet3_softc *);

static void vmxnet3_enable_all_intrs(struct vmxnet3_softc *);
static void vmxnet3_disable_all_intrs(struct vmxnet3_softc *);

static int vmxnet3_dma_malloc(struct vmxnet3_softc *, bus_size_t, bus_size_t,
    struct vmxnet3_dma_alloc *);
static void vmxnet3_dma_free(struct vmxnet3_softc *, struct vmxnet3_dma_alloc *);

CFATTACH_DECL3_NEW(vmx, sizeof(struct vmxnet3_softc),
    vmxnet3_match, vmxnet3_attach, vmxnet3_detach, NULL, NULL, NULL, 0);

/* round down to the nearest power of 2 */
static int
vmxnet3_calc_queue_size(int n)
{

	if (__predict_false(n <= 0))
		return 1;

	return (1U << (fls32(n) - 1));
}

static inline void
vmxnet3_write_bar0(struct vmxnet3_softc *sc, bus_size_t r, uint32_t v)
{

	bus_space_write_4(sc->vmx_iot0, sc->vmx_ioh0, r, v);
}

static inline uint32_t
vmxnet3_read_bar1(struct vmxnet3_softc *sc, bus_size_t r)
{

	return (bus_space_read_4(sc->vmx_iot1, sc->vmx_ioh1, r));
}

static inline void
vmxnet3_write_bar1(struct vmxnet3_softc *sc, bus_size_t r, uint32_t v)
{

	bus_space_write_4(sc->vmx_iot1, sc->vmx_ioh1, r, v);
}

static inline void
vmxnet3_write_cmd(struct vmxnet3_softc *sc, uint32_t cmd)
{

	vmxnet3_write_bar1(sc, VMXNET3_BAR1_CMD, cmd);
}

static inline uint32_t
vmxnet3_read_cmd(struct vmxnet3_softc *sc, uint32_t cmd)
{

	vmxnet3_write_cmd(sc, cmd);
	return (vmxnet3_read_bar1(sc, VMXNET3_BAR1_CMD));
}

static inline void
vmxnet3_enable_intr(struct vmxnet3_softc *sc, int irq)
{
	vmxnet3_write_bar0(sc, VMXNET3_BAR0_IMASK(irq), 0);
}

static inline void
vmxnet3_disable_intr(struct vmxnet3_softc *sc, int irq)
{
	vmxnet3_write_bar0(sc, VMXNET3_BAR0_IMASK(irq), 1);
}

static inline void
vmxnet3_rxr_increment_fill(struct vmxnet3_rxring *rxr)
{

	if (++rxr->vxrxr_fill == rxr->vxrxr_ndesc) {
		rxr->vxrxr_fill = 0;
		rxr->vxrxr_gen ^= 1;
	}
}

static inline int
vmxnet3_txring_avail(struct vmxnet3_txring *txr)
{
	int avail = txr->vxtxr_next - txr->vxtxr_head - 1;
	return (avail < 0 ? (int)txr->vxtxr_ndesc + avail : avail);
}

/*
 * Since this is a purely paravirtualized device, we do not have
 * to worry about DMA coherency. But at times, we must make sure
 * both the compiler and CPU do not reorder memory operations.
 */
static inline void
vmxnet3_barrier(struct vmxnet3_softc *sc, vmxnet3_barrier_t type)
{

	switch (type) {
	case VMXNET3_BARRIER_RD:
		membar_consumer();
		break;
	case VMXNET3_BARRIER_WR:
		membar_producer();
		break;
	default:
		panic("%s: bad barrier type %d", __func__, type);
	}
}

static int
vmxnet3_match(device_t parent, cfdata_t match, void *aux)
{
	struct pci_attach_args *pa = (struct pci_attach_args *)aux;

	if (PCI_VENDOR(pa->pa_id) == PCI_VENDOR_VMWARE &&
	    PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_VMWARE_VMXNET3)
		return 1;

	return 0;
}

static void
vmxnet3_attach(device_t parent, device_t self, void *aux)
{
	struct vmxnet3_softc *sc = device_private(self);
	struct pci_attach_args *pa = aux;
	pcireg_t preg;
	int error;
	int candidate;

	sc->vmx_dev = self;
	sc->vmx_pa = pa;
	sc->vmx_pc = pa->pa_pc;
	if (pci_dma64_available(pa))
		sc->vmx_dmat = pa->pa_dmat64;
	else
		sc->vmx_dmat = pa->pa_dmat;

	pci_aprint_devinfo_fancy(pa, "Ethernet controller", "vmxnet3", 1);

	preg = pci_conf_read(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);
	preg |= PCI_COMMAND_MASTER_ENABLE;
	pci_conf_write(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG, preg);

	sc->vmx_mtx = mutex_obj_alloc(MUTEX_DEFAULT, IPL_NET);
	callout_init(&sc->vmx_tick, CALLOUT_MPSAFE);

	candidate = MIN(MIN(VMXNET3_MAX_TX_QUEUES, VMXNET3_MAX_RX_QUEUES),
	    ncpu);
	sc->vmx_max_ntxqueues = sc->vmx_max_nrxqueues =
	    vmxnet3_calc_queue_size(candidate);
	sc->vmx_ntxdescs = 512;
	sc->vmx_nrxdescs = 256;
	sc->vmx_max_rxsegs = VMXNET3_MAX_RX_SEGS;

	error = vmxnet3_alloc_pci_resources(sc);
	if (error)
		return;

	error = vmxnet3_check_version(sc);
	if (error)
		return;

	error = vmxnet3_alloc_rxtx_queues(sc);
	if (error)
		return;

	error = vmxnet3_alloc_interrupts(sc);
	if (error)
		return;

	vmxnet3_check_multiqueue(sc);

	error = vmxnet3_alloc_data(sc);
	if (error)
		return;

	error = vmxnet3_setup_interface(sc);
	if (error)
		return;

	error = vmxnet3_setup_interrupts(sc);
	if (error)
		return;

	error = vmxnet3_setup_sysctl(sc);
	if (error)
		return;

	error = vmxnet3_setup_stats(sc);
	if (error)
		return;

	sc->vmx_flags |= VMXNET3_FLAG_ATTACHED;
}

static int
vmxnet3_detach(device_t self, int flags)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;

	sc = device_private(self);
	ifp = &sc->vmx_ethercom.ec_if;

	if (sc->vmx_flags & VMXNET3_FLAG_ATTACHED) {
		VMXNET3_CORE_LOCK(sc);
		vmxnet3_stop_locked(sc);
		callout_halt(&sc->vmx_tick, sc->vmx_mtx);
		callout_destroy(&sc->vmx_tick);
		VMXNET3_CORE_UNLOCK(sc);

		ether_ifdetach(ifp);
		if_detach(ifp);
		ifmedia_fini(&sc->vmx_media);
	}

	vmxnet3_teardown_stats(sc);
	sysctl_teardown(&sc->vmx_sysctllog);

	vmxnet3_free_interrupts(sc);

	vmxnet3_free_data(sc);
	vmxnet3_free_pci_resources(sc);
	vmxnet3_free_rxtx_queues(sc);

	if (sc->vmx_mtx)
		mutex_obj_free(sc->vmx_mtx);

	return (0);
}

static int
vmxnet3_alloc_pci_resources(struct vmxnet3_softc *sc)
{
	struct pci_attach_args *pa = sc->vmx_pa;
	pcireg_t memtype;

	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_BAR(0));
	if (pci_mapreg_map(pa, PCI_BAR(0), memtype, 0, &sc->vmx_iot0, &sc->vmx_ioh0,
	    NULL, &sc->vmx_ios0)) {
		aprint_error_dev(sc->vmx_dev, "failed to map BAR0\n");
		return (ENXIO);
	}
	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_BAR(1));
	if (pci_mapreg_map(pa, PCI_BAR(1), memtype, 0, &sc->vmx_iot1, &sc->vmx_ioh1,
	    NULL, &sc->vmx_ios1)) {
		aprint_error_dev(sc->vmx_dev, "failed to map BAR1\n");
		return (ENXIO);
	}

	if (!pci_get_capability(pa->pa_pc, pa->pa_tag, PCI_CAP_MSIX, NULL, NULL)) {
		sc->vmx_flags |= VMXNET3_FLAG_NO_MSIX;
		return (0);
	}

	return (0);
}

static void
vmxnet3_free_pci_resources(struct vmxnet3_softc *sc)
{

	if (sc->vmx_ios0) {
		bus_space_unmap(sc->vmx_iot0, sc->vmx_ioh0, sc->vmx_ios0);
		sc->vmx_ios0 = 0;
	}

	if (sc->vmx_ios1) {
		bus_space_unmap(sc->vmx_iot1, sc->vmx_ioh1, sc->vmx_ios1);
		sc->vmx_ios1 = 0;
	}
}

static int
vmxnet3_check_version(struct vmxnet3_softc *sc)
{
	u_int ver;

	ver = vmxnet3_read_bar1(sc, VMXNET3_BAR1_VRRS);
	if ((ver & 0x1) == 0) {
		aprint_error_dev(sc->vmx_dev,
		    "unsupported hardware version 0x%x\n", ver);
		return (ENOTSUP);
	}
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_VRRS, 1);

	ver = vmxnet3_read_bar1(sc, VMXNET3_BAR1_UVRS);
	if ((ver & 0x1) == 0) {
		aprint_error_dev(sc->vmx_dev,
		    "incompatiable UPT version 0x%x\n", ver);
		return (ENOTSUP);
	}
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_UVRS, 1);

	return (0);
}

static void
vmxnet3_check_multiqueue(struct vmxnet3_softc *sc)
{

	if (sc->vmx_intr_type != VMXNET3_IT_MSIX)
		goto out;

	/* Just use the maximum configured for now. */
	sc->vmx_nrxqueues = sc->vmx_max_nrxqueues;
	sc->vmx_ntxqueues = sc->vmx_max_ntxqueues;

	if (sc->vmx_nrxqueues > 1)
		sc->vmx_flags |= VMXNET3_FLAG_RSS;

	return;

out:
	sc->vmx_ntxqueues = 1;
	sc->vmx_nrxqueues = 1;
}

static int
vmxnet3_alloc_msix_interrupts(struct vmxnet3_softc *sc)
{
	int required;
	struct pci_attach_args *pa = sc->vmx_pa;

	if (sc->vmx_flags & VMXNET3_FLAG_NO_MSIX)
		return (1);

	/* Allocate an additional vector for the events interrupt. */
	required = MIN(sc->vmx_max_ntxqueues, sc->vmx_max_nrxqueues) + 1;

	if (pci_msix_count(pa->pa_pc, pa->pa_tag) < required)
		return (1);

	if (pci_msix_alloc_exact(pa, &sc->vmx_intrs, required) == 0) {
		sc->vmx_nintrs = required;
		return (0);
	}

	return (1);
}

static int
vmxnet3_alloc_msi_interrupts(struct vmxnet3_softc *sc)
{
	int nmsi, required;
	struct pci_attach_args *pa = sc->vmx_pa;

	required = 1;

	nmsi = pci_msi_count(pa->pa_pc, pa->pa_tag);
	if (nmsi < required)
		return (1);

	if (pci_msi_alloc_exact(pa, &sc->vmx_intrs, required) == 0) {
		sc->vmx_nintrs = required;
		return (0);
	}

	return (1);
}

static int
vmxnet3_alloc_legacy_interrupts(struct vmxnet3_softc *sc)
{

	if (pci_intx_alloc(sc->vmx_pa, &sc->vmx_intrs) == 0) {
		sc->vmx_nintrs = 1;
		return (0);
	}

	return (1);
}

static int
vmxnet3_alloc_interrupts(struct vmxnet3_softc *sc)
{
	u_int config;
	int error;

	config = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_INTRCFG);

	sc->vmx_intr_type = config & 0x03;
	sc->vmx_intr_mask_mode = (config >> 2) & 0x03;

	switch (sc->vmx_intr_type) {
	case VMXNET3_IT_AUTO:
		sc->vmx_intr_type = VMXNET3_IT_MSIX;
		/* FALLTHROUGH */
	case VMXNET3_IT_MSIX:
		error = vmxnet3_alloc_msix_interrupts(sc);
		if (error == 0)
			break;
		sc->vmx_intr_type = VMXNET3_IT_MSI;
		/* FALLTHROUGH */
	case VMXNET3_IT_MSI:
		error = vmxnet3_alloc_msi_interrupts(sc);
		if (error == 0)
			break;
		sc->vmx_intr_type = VMXNET3_IT_LEGACY;
		/* FALLTHROUGH */
	case VMXNET3_IT_LEGACY:
		error = vmxnet3_alloc_legacy_interrupts(sc);
		if (error == 0)
			break;
		/* FALLTHROUGH */
	default:
		sc->vmx_intr_type = -1;
		aprint_error_dev(sc->vmx_dev, "cannot allocate any interrupt resources\n");
		return (ENXIO);
	}

	return (error);
}

static void
vmxnet3_free_interrupts(struct vmxnet3_softc *sc)
{
	pci_chipset_tag_t pc = sc->vmx_pc;
	int i;

	workqueue_destroy(sc->vmx_queue_wq);
	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		struct vmxnet3_queue *vmxq =  &sc->vmx_queue[i];

		softint_disestablish(vmxq->vxq_si);
		vmxq->vxq_si = NULL;
	}
	for (i = 0; i < sc->vmx_nintrs; i++) {
		pci_intr_disestablish(pc, sc->vmx_ihs[i]);
	}
	pci_intr_release(pc, sc->vmx_intrs, sc->vmx_nintrs);
}

static int
vmxnet3_setup_msix_interrupts(struct vmxnet3_softc *sc)
{
	pci_chipset_tag_t pc = sc->vmx_pa->pa_pc;
	struct vmxnet3_queue *vmxq;
	pci_intr_handle_t *intr;
	void **ihs;
	int intr_idx, i, use_queues, error;
	kcpuset_t *affinity;
	const char *intrstr;
	char intrbuf[PCI_INTRSTR_LEN];
	char xnamebuf[32];

	intr = sc->vmx_intrs;
	intr_idx = 0;
	ihs = sc->vmx_ihs;

	/* See vmxnet3_alloc_msix_interrupts() */
	use_queues = MIN(sc->vmx_max_ntxqueues, sc->vmx_max_nrxqueues);
	for (i = 0; i < use_queues; i++, intr++, ihs++, intr_idx++) {
		snprintf(xnamebuf, 32, "%s: txrx %d", device_xname(sc->vmx_dev), i);

		vmxq = &sc->vmx_queue[i];

		intrstr = pci_intr_string(pc, *intr, intrbuf, sizeof(intrbuf));

		pci_intr_setattr(pc, intr, PCI_INTR_MPSAFE, true);
		*ihs = pci_intr_establish_xname(pc, *intr, IPL_NET,
		    vmxnet3_txrxq_intr, vmxq, xnamebuf);
		if (*ihs == NULL) {
			aprint_error_dev(sc->vmx_dev,
			    "unable to establish txrx interrupt at %s\n", intrstr);
			return (-1);
		}
		aprint_normal_dev(sc->vmx_dev, "txrx interrupting at %s\n", intrstr);

		kcpuset_create(&affinity, true);
		kcpuset_set(affinity, intr_idx % ncpu);
		error = interrupt_distribute(*ihs, affinity, NULL);
		if (error) {
			aprint_normal_dev(sc->vmx_dev,
			    "%s cannot be changed affinity, use default CPU\n",
			    intrstr);
		}
		kcpuset_destroy(affinity);

		vmxq->vxq_si = softint_establish(SOFTINT_NET | SOFTINT_MPSAFE,
		    vmxnet3_handle_queue, vmxq);
		if (vmxq->vxq_si == NULL) {
			aprint_error_dev(sc->vmx_dev,
			    "softint_establish for vxq_si failed\n");
			return (-1);
		}

		vmxq->vxq_intr_idx = intr_idx;
	}
	snprintf(xnamebuf, MAXCOMLEN, "%s_tx_rx", device_xname(sc->vmx_dev));
	error = workqueue_create(&sc->vmx_queue_wq, xnamebuf,
	    vmxnet3_handle_queue_work, sc, VMXNET3_WORKQUEUE_PRI, IPL_NET,
	    WQ_PERCPU | WQ_MPSAFE);
	if (error) {
		aprint_error_dev(sc->vmx_dev, "workqueue_create failed\n");
		return (-1);
	}
	sc->vmx_txrx_workqueue = false;

	intrstr = pci_intr_string(pc, *intr, intrbuf, sizeof(intrbuf));

	snprintf(xnamebuf, 32, "%s: link", device_xname(sc->vmx_dev));
	pci_intr_setattr(pc, intr, PCI_INTR_MPSAFE, true);
	*ihs = pci_intr_establish_xname(pc, *intr, IPL_NET,
	    vmxnet3_event_intr, sc, xnamebuf);
	if (*ihs == NULL) {
		aprint_error_dev(sc->vmx_dev,
		    "unable to establish event interrupt at %s\n", intrstr);
		return (-1);
	}
	aprint_normal_dev(sc->vmx_dev, "event interrupting at %s\n", intrstr);

	sc->vmx_event_intr_idx = intr_idx;

	return (0);
}

static int
vmxnet3_setup_msi_interrupt(struct vmxnet3_softc *sc)
{
	pci_chipset_tag_t pc = sc->vmx_pa->pa_pc;
	pci_intr_handle_t *intr;
	void **ihs;
	struct vmxnet3_queue *vmxq;
	int i;
	const char *intrstr;
	char intrbuf[PCI_INTRSTR_LEN];
	char xnamebuf[32];

	intr = &sc->vmx_intrs[0];
	ihs = sc->vmx_ihs;
	vmxq = &sc->vmx_queue[0];

	intrstr = pci_intr_string(pc, *intr, intrbuf, sizeof(intrbuf));

	snprintf(xnamebuf, 32, "%s: msi", device_xname(sc->vmx_dev));
	pci_intr_setattr(pc, intr, PCI_INTR_MPSAFE, true);
	*ihs = pci_intr_establish_xname(pc, *intr, IPL_NET,
	    vmxnet3_legacy_intr, sc, xnamebuf);
	if (*ihs == NULL) {
		aprint_error_dev(sc->vmx_dev,
		    "unable to establish interrupt at %s\n", intrstr);
		return (-1);
	}
	aprint_normal_dev(sc->vmx_dev, "interrupting at %s\n", intrstr);

	vmxq->vxq_si = softint_establish(SOFTINT_NET | SOFTINT_MPSAFE,
	    vmxnet3_handle_queue, vmxq);
	if (vmxq->vxq_si == NULL) {
		aprint_error_dev(sc->vmx_dev,
		    "softint_establish for vxq_si failed\n");
		return (-1);
	}

	for (i = 0; i < MIN(sc->vmx_nrxqueues, sc->vmx_nrxqueues); i++)
		sc->vmx_queue[i].vxq_intr_idx = 0;
	sc->vmx_event_intr_idx = 0;

	return (0);
}

static int
vmxnet3_setup_legacy_interrupt(struct vmxnet3_softc *sc)
{
	pci_chipset_tag_t pc = sc->vmx_pa->pa_pc;
	pci_intr_handle_t *intr;
	void **ihs;
	struct vmxnet3_queue *vmxq;
	int i;
	const char *intrstr;
	char intrbuf[PCI_INTRSTR_LEN];
	char xnamebuf[32];

	intr = &sc->vmx_intrs[0];
	ihs = sc->vmx_ihs;
	vmxq = &sc->vmx_queue[0];

	intrstr = pci_intr_string(pc, *intr, intrbuf, sizeof(intrbuf));

	snprintf(xnamebuf, 32, "%s:legacy", device_xname(sc->vmx_dev));
	pci_intr_setattr(pc, intr, PCI_INTR_MPSAFE, true);
	*ihs = pci_intr_establish_xname(pc, *intr, IPL_NET,
	    vmxnet3_legacy_intr, sc, xnamebuf);
	if (*ihs == NULL) {
		aprint_error_dev(sc->vmx_dev,
		    "unable to establish interrupt at %s\n", intrstr);
		return (-1);
	}
	aprint_normal_dev(sc->vmx_dev, "interrupting at %s\n", intrstr);

	vmxq->vxq_si = softint_establish(SOFTINT_NET | SOFTINT_MPSAFE,
	    vmxnet3_handle_queue, vmxq);
	if (vmxq->vxq_si == NULL) {
		aprint_error_dev(sc->vmx_dev,
		    "softint_establish for vxq_si failed\n");
		return (-1);
	}

	for (i = 0; i < MIN(sc->vmx_nrxqueues, sc->vmx_nrxqueues); i++)
		sc->vmx_queue[i].vxq_intr_idx = 0;
	sc->vmx_event_intr_idx = 0;

	return (0);
}

static void
vmxnet3_set_interrupt_idx(struct vmxnet3_softc *sc)
{
	struct vmxnet3_queue *vmxq;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txq_shared *txs;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxq_shared *rxs;
	int i;

	sc->vmx_ds->evintr = sc->vmx_event_intr_idx;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		vmxq = &sc->vmx_queue[i];
		txq = &vmxq->vxq_txqueue;
		txs = txq->vxtxq_ts;
		txs->intr_idx = vmxq->vxq_intr_idx;
	}

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		vmxq = &sc->vmx_queue[i];
		rxq = &vmxq->vxq_rxqueue;
		rxs = rxq->vxrxq_rs;
		rxs->intr_idx = vmxq->vxq_intr_idx;
	}
}

static int
vmxnet3_setup_interrupts(struct vmxnet3_softc *sc)
{
	int error;

	switch (sc->vmx_intr_type) {
	case VMXNET3_IT_MSIX:
		error = vmxnet3_setup_msix_interrupts(sc);
		break;
	case VMXNET3_IT_MSI:
		error = vmxnet3_setup_msi_interrupt(sc);
		break;
	case VMXNET3_IT_LEGACY:
		error = vmxnet3_setup_legacy_interrupt(sc);
		break;
	default:
		panic("%s: invalid interrupt type %d", __func__,
		    sc->vmx_intr_type);
	}

	if (error == 0)
		vmxnet3_set_interrupt_idx(sc);

	return (error);
}

static int
vmxnet3_init_rxq(struct vmxnet3_softc *sc, int q)
{
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	int i;

	rxq = &sc->vmx_queue[q].vxq_rxqueue;

	snprintf(rxq->vxrxq_name, sizeof(rxq->vxrxq_name), "%s-rx%d",
	    device_xname(sc->vmx_dev), q);
	rxq->vxrxq_mtx = mutex_obj_alloc(MUTEX_DEFAULT, IPL_NET /* XXX */);

	rxq->vxrxq_sc = sc;

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_rid = i;
		rxr->vxrxr_ndesc = sc->vmx_nrxdescs;
		rxr->vxrxr_rxbuf = kmem_zalloc(rxr->vxrxr_ndesc *
		    sizeof(struct vmxnet3_rxbuf), KM_SLEEP);

		rxq->vxrxq_comp_ring.vxcr_ndesc += sc->vmx_nrxdescs;
	}

	return (0);
}

static int
vmxnet3_init_txq(struct vmxnet3_softc *sc, int q)
{
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;

	txq = &sc->vmx_queue[q].vxq_txqueue;
	txr = &txq->vxtxq_cmd_ring;

	snprintf(txq->vxtxq_name, sizeof(txq->vxtxq_name), "%s-tx%d",
	    device_xname(sc->vmx_dev), q);
	txq->vxtxq_mtx = mutex_obj_alloc(MUTEX_DEFAULT, IPL_NET /* XXX */);

	txq->vxtxq_sc = sc;

	txq->vxtxq_si = softint_establish(SOFTINT_NET | SOFTINT_MPSAFE,
	    vmxnet3_deferred_transmit, txq);
	if (txq->vxtxq_si == NULL) {
		mutex_obj_free(txq->vxtxq_mtx);
		aprint_error_dev(sc->vmx_dev,
		    "softint_establish for vxtxq_si failed\n");
		return ENOMEM;
	}

	txr->vxtxr_ndesc = sc->vmx_ntxdescs;
	txr->vxtxr_txbuf = kmem_zalloc(txr->vxtxr_ndesc *
	    sizeof(struct vmxnet3_txbuf), KM_SLEEP);

	txq->vxtxq_comp_ring.vxcr_ndesc = sc->vmx_ntxdescs;

	txq->vxtxq_interq = pcq_create(sc->vmx_ntxdescs, KM_SLEEP);

	return (0);
}

static int
vmxnet3_alloc_rxtx_queues(struct vmxnet3_softc *sc)
{
	int i, error, max_nqueues;

	KASSERT(!cpu_intr_p());
	KASSERT(!cpu_softintr_p());

	/*
	 * Only attempt to create multiple queues if MSIX is available.
	 * This check prevents us from allocating queue structures that
	 * we will not use.
	 *
	 * FreeBSD:
	 * MSIX is disabled by default because its apparently broken for
	 * devices passed through by at least ESXi 5.1.
	 * The hw.pci.honor_msi_blacklist tunable must be set to zero for MSIX.
	 */
	if (sc->vmx_flags & VMXNET3_FLAG_NO_MSIX) {
		sc->vmx_max_nrxqueues = 1;
		sc->vmx_max_ntxqueues = 1;
	}

	max_nqueues = MAX(sc->vmx_max_ntxqueues, sc->vmx_max_nrxqueues);
	sc->vmx_queue = kmem_zalloc(sizeof(struct vmxnet3_queue) * max_nqueues,
	    KM_SLEEP);

	for (i = 0; i < max_nqueues; i++) {
		struct vmxnet3_queue *vmxq = &sc->vmx_queue[i];
		vmxq->vxq_id = i;
	}

	for (i = 0; i < sc->vmx_max_nrxqueues; i++) {
		error = vmxnet3_init_rxq(sc, i);
		if (error)
			return (error);
	}

	for (i = 0; i < sc->vmx_max_ntxqueues; i++) {
		error = vmxnet3_init_txq(sc, i);
		if (error)
			return (error);
	}

	return (0);
}

static void
vmxnet3_destroy_rxq(struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_rxring *rxr;
	int i;

	rxq->vxrxq_sc = NULL;

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];

		if (rxr->vxrxr_rxbuf != NULL) {
			kmem_free(rxr->vxrxr_rxbuf,
			    rxr->vxrxr_ndesc * sizeof(struct vmxnet3_rxbuf));
			rxr->vxrxr_rxbuf = NULL;
		}
	}

	if (rxq->vxrxq_mtx != NULL)
		mutex_obj_free(rxq->vxrxq_mtx);
}

static void
vmxnet3_destroy_txq(struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;
	struct mbuf *m;

	txr = &txq->vxtxq_cmd_ring;

	txq->vxtxq_sc = NULL;

	softint_disestablish(txq->vxtxq_si);

	while ((m = pcq_get(txq->vxtxq_interq)) != NULL)
		m_freem(m);
	pcq_destroy(txq->vxtxq_interq);

	if (txr->vxtxr_txbuf != NULL) {
		kmem_free(txr->vxtxr_txbuf,
		    txr->vxtxr_ndesc * sizeof(struct vmxnet3_txbuf));
		txr->vxtxr_txbuf = NULL;
	}

	if (txq->vxtxq_mtx != NULL)
		mutex_obj_free(txq->vxtxq_mtx);
}

static void
vmxnet3_free_rxtx_queues(struct vmxnet3_softc *sc)
{
	int i;

	if (sc->vmx_queue != NULL) {
		int max_nqueues;

		for (i = 0; i < sc->vmx_max_nrxqueues; i++)
			vmxnet3_destroy_rxq(&sc->vmx_queue[i].vxq_rxqueue);

		for (i = 0; i < sc->vmx_max_ntxqueues; i++)
			vmxnet3_destroy_txq(&sc->vmx_queue[i].vxq_txqueue);

		max_nqueues = MAX(sc->vmx_max_nrxqueues, sc->vmx_max_ntxqueues);
		kmem_free(sc->vmx_queue,
		    sizeof(struct vmxnet3_queue) * max_nqueues);
	}
}

static int
vmxnet3_alloc_shared_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	uint8_t *kva;
	size_t size;
	int i, error;

	dev = sc->vmx_dev;

	size = sizeof(struct vmxnet3_driver_shared);
	error = vmxnet3_dma_malloc(sc, size, 1, &sc->vmx_ds_dma);
	if (error) {
		device_printf(dev, "cannot alloc shared memory\n");
		return (error);
	}
	sc->vmx_ds = (struct vmxnet3_driver_shared *) sc->vmx_ds_dma.dma_vaddr;

	size = sc->vmx_ntxqueues * sizeof(struct vmxnet3_txq_shared) +
	    sc->vmx_nrxqueues * sizeof(struct vmxnet3_rxq_shared);
	error = vmxnet3_dma_malloc(sc, size, 128, &sc->vmx_qs_dma);
	if (error) {
		device_printf(dev, "cannot alloc queue shared memory\n");
		return (error);
	}
	sc->vmx_qs = (void *) sc->vmx_qs_dma.dma_vaddr;
	kva = sc->vmx_qs;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		sc->vmx_queue[i].vxq_txqueue.vxtxq_ts =
		    (struct vmxnet3_txq_shared *) kva;
		kva += sizeof(struct vmxnet3_txq_shared);
	}
	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		sc->vmx_queue[i].vxq_rxqueue.vxrxq_rs =
		    (struct vmxnet3_rxq_shared *) kva;
		kva += sizeof(struct vmxnet3_rxq_shared);
	}

	if (sc->vmx_flags & VMXNET3_FLAG_RSS) {
		size = sizeof(struct vmxnet3_rss_shared);
		error = vmxnet3_dma_malloc(sc, size, 128, &sc->vmx_rss_dma);
		if (error) {
			device_printf(dev, "cannot alloc rss shared memory\n");
			return (error);
		}
		sc->vmx_rss =
		    (struct vmxnet3_rss_shared *) sc->vmx_rss_dma.dma_vaddr;
	}

	return (0);
}

static void
vmxnet3_free_shared_data(struct vmxnet3_softc *sc)
{

	if (sc->vmx_rss != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_rss_dma);
		sc->vmx_rss = NULL;
	}

	if (sc->vmx_qs != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_qs_dma);
		sc->vmx_qs = NULL;
	}

	if (sc->vmx_ds != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_ds_dma);
		sc->vmx_ds = NULL;
	}
}

static int
vmxnet3_alloc_txq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	size_t descsz, compsz;
	u_int i;
	int q, error;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_ntxqueues; q++) {
		txq = &sc->vmx_queue[q].vxq_txqueue;
		txr = &txq->vxtxq_cmd_ring;
		txc = &txq->vxtxq_comp_ring;

		descsz = txr->vxtxr_ndesc * sizeof(struct vmxnet3_txdesc);
		compsz = txr->vxtxr_ndesc * sizeof(struct vmxnet3_txcompdesc);

		error = vmxnet3_dma_malloc(sc, descsz, 512, &txr->vxtxr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Tx descriptors for "
			    "queue %d error %d\n", q, error);
			return (error);
		}
		txr->vxtxr_txd =
		    (struct vmxnet3_txdesc *) txr->vxtxr_dma.dma_vaddr;

		error = vmxnet3_dma_malloc(sc, compsz, 512, &txc->vxcr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Tx comp descriptors "
			   "for queue %d error %d\n", q, error);
			return (error);
		}
		txc->vxcr_u.txcd =
		    (struct vmxnet3_txcompdesc *) txc->vxcr_dma.dma_vaddr;

		for (i = 0; i < txr->vxtxr_ndesc; i++) {
			error = bus_dmamap_create(sc->vmx_dmat, VMXNET3_TX_MAXSIZE,
			    VMXNET3_TX_MAXSEGS, VMXNET3_TX_MAXSEGSIZE, 0, BUS_DMA_NOWAIT,
			    &txr->vxtxr_txbuf[i].vtxb_dmamap);
			if (error) {
				device_printf(dev, "unable to create Tx buf "
				    "dmamap for queue %d idx %d\n", q, i);
				return (error);
			}
		}
	}

	return (0);
}

static void
vmxnet3_free_txq_data(struct vmxnet3_softc *sc)
{
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	struct vmxnet3_txbuf *txb;
	u_int i;
	int q;

	for (q = 0; q < sc->vmx_ntxqueues; q++) {
		txq = &sc->vmx_queue[q].vxq_txqueue;
		txr = &txq->vxtxq_cmd_ring;
		txc = &txq->vxtxq_comp_ring;

		for (i = 0; i < txr->vxtxr_ndesc; i++) {
			txb = &txr->vxtxr_txbuf[i];
			if (txb->vtxb_dmamap != NULL) {
				bus_dmamap_destroy(sc->vmx_dmat,
				    txb->vtxb_dmamap);
				txb->vtxb_dmamap = NULL;
			}
		}

		if (txc->vxcr_u.txcd != NULL) {
			vmxnet3_dma_free(sc, &txc->vxcr_dma);
			txc->vxcr_u.txcd = NULL;
		}

		if (txr->vxtxr_txd != NULL) {
			vmxnet3_dma_free(sc, &txr->vxtxr_dma);
			txr->vxtxr_txd = NULL;
		}
	}
}

static int
vmxnet3_alloc_rxq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	int descsz, compsz;
	u_int i, j;
	int q, error;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		rxq = &sc->vmx_queue[q].vxq_rxqueue;
		rxc = &rxq->vxrxq_comp_ring;
		compsz = 0;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			descsz = rxr->vxrxr_ndesc *
			    sizeof(struct vmxnet3_rxdesc);
			compsz += rxr->vxrxr_ndesc *
			    sizeof(struct vmxnet3_rxcompdesc);

			error = vmxnet3_dma_malloc(sc, descsz, 512,
			    &rxr->vxrxr_dma);
			if (error) {
				device_printf(dev, "cannot allocate Rx "
				    "descriptors for queue %d/%d error %d\n",
				    i, q, error);
				return (error);
			}
			rxr->vxrxr_rxd =
			    (struct vmxnet3_rxdesc *) rxr->vxrxr_dma.dma_vaddr;
		}

		error = vmxnet3_dma_malloc(sc, compsz, 512, &rxc->vxcr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Rx comp descriptors "
			    "for queue %d error %d\n", q, error);
			return (error);
		}
		rxc->vxcr_u.rxcd =
		    (struct vmxnet3_rxcompdesc *) rxc->vxcr_dma.dma_vaddr;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			error = bus_dmamap_create(sc->vmx_dmat, JUMBO_LEN, 1,
			    JUMBO_LEN, 0, BUS_DMA_NOWAIT,
			    &rxr->vxrxr_spare_dmap);
			if (error) {
				device_printf(dev, "unable to create spare "
				    "dmamap for queue %d/%d error %d\n",
				    q, i, error);
				return (error);
			}

			for (j = 0; j < rxr->vxrxr_ndesc; j++) {
				error = bus_dmamap_create(sc->vmx_dmat, JUMBO_LEN, 1,
				    JUMBO_LEN, 0, BUS_DMA_NOWAIT,
				    &rxr->vxrxr_rxbuf[j].vrxb_dmamap);
				if (error) {
					device_printf(dev, "unable to create "
					    "dmamap for queue %d/%d slot %d "
					    "error %d\n",
					    q, i, j, error);
					return (error);
				}
			}
		}
	}

	return (0);
}

static void
vmxnet3_free_rxq_data(struct vmxnet3_softc *sc)
{
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	struct vmxnet3_rxbuf *rxb;
	u_int i, j;
	int q;

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		rxq = &sc->vmx_queue[q].vxq_rxqueue;
		rxc = &rxq->vxrxq_comp_ring;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			if (rxr->vxrxr_spare_dmap != NULL) {
				bus_dmamap_destroy(sc->vmx_dmat,
				    rxr->vxrxr_spare_dmap);
				rxr->vxrxr_spare_dmap = NULL;
			}

			for (j = 0; j < rxr->vxrxr_ndesc; j++) {
				rxb = &rxr->vxrxr_rxbuf[j];
				if (rxb->vrxb_dmamap != NULL) {
					bus_dmamap_destroy(sc->vmx_dmat,
					    rxb->vrxb_dmamap);
					rxb->vrxb_dmamap = NULL;
				}
			}
		}

		if (rxc->vxcr_u.rxcd != NULL) {
			vmxnet3_dma_free(sc, &rxc->vxcr_dma);
			rxc->vxcr_u.rxcd = NULL;
		}

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			if (rxr->vxrxr_rxd != NULL) {
				vmxnet3_dma_free(sc, &rxr->vxrxr_dma);
				rxr->vxrxr_rxd = NULL;
			}
		}
	}
}

static int
vmxnet3_alloc_queue_data(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_alloc_txq_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_rxq_data(sc);
	if (error)
		return (error);

	return (0);
}

static void
vmxnet3_free_queue_data(struct vmxnet3_softc *sc)
{

	if (sc->vmx_queue != NULL) {
		vmxnet3_free_rxq_data(sc);
		vmxnet3_free_txq_data(sc);
	}
}

static int
vmxnet3_alloc_mcast_table(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_dma_malloc(sc, VMXNET3_MULTICAST_MAX * ETHER_ADDR_LEN,
	    32, &sc->vmx_mcast_dma);
	if (error)
		device_printf(sc->vmx_dev, "unable to alloc multicast table\n");
	else
		sc->vmx_mcast = sc->vmx_mcast_dma.dma_vaddr;

	return (error);
}

static void
vmxnet3_free_mcast_table(struct vmxnet3_softc *sc)
{

	if (sc->vmx_mcast != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_mcast_dma);
		sc->vmx_mcast = NULL;
	}
}

static void
vmxnet3_init_shared_data(struct vmxnet3_softc *sc)
{
	struct vmxnet3_driver_shared *ds;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txq_shared *txs;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxq_shared *rxs;
	int i;

	ds = sc->vmx_ds;

	/*
	 * Initialize fields of the shared data that remains the same across
	 * reinits. Note the shared data is zero'd when allocated.
	 */

	ds->magic = VMXNET3_REV1_MAGIC;

	/* DriverInfo */
	ds->version = VMXNET3_DRIVER_VERSION;
	ds->guest = VMXNET3_GOS_FREEBSD |
#ifdef __LP64__
	    VMXNET3_GOS_64BIT;
#else
	    VMXNET3_GOS_32BIT;
#endif
	ds->vmxnet3_revision = 1;
	ds->upt_version = 1;

	/* Misc. conf */
	ds->driver_data = vtophys(sc);
	ds->driver_data_len = sizeof(struct vmxnet3_softc);
	ds->queue_shared = sc->vmx_qs_dma.dma_paddr;
	ds->queue_shared_len = sc->vmx_qs_dma.dma_size;
	ds->nrxsg_max = sc->vmx_max_rxsegs;

	/* RSS conf */
	if (sc->vmx_flags & VMXNET3_FLAG_RSS) {
		ds->rss.version = 1;
		ds->rss.paddr = sc->vmx_rss_dma.dma_paddr;
		ds->rss.len = sc->vmx_rss_dma.dma_size;
	}

	/* Interrupt control. */
	ds->automask = sc->vmx_intr_mask_mode == VMXNET3_IMM_AUTO;
	ds->nintr = sc->vmx_nintrs;
	ds->evintr = sc->vmx_event_intr_idx;
	ds->ictrl = VMXNET3_ICTRL_DISABLE_ALL;

	for (i = 0; i < sc->vmx_nintrs; i++)
		ds->modlevel[i] = UPT1_IMOD_ADAPTIVE;

	/* Receive filter. */
	ds->mcast_table = sc->vmx_mcast_dma.dma_paddr;
	ds->mcast_tablelen = sc->vmx_mcast_dma.dma_size;

	/* Tx queues */
	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_queue[i].vxq_txqueue;
		txs = txq->vxtxq_ts;

		txs->cmd_ring = txq->vxtxq_cmd_ring.vxtxr_dma.dma_paddr;
		txs->cmd_ring_len = txq->vxtxq_cmd_ring.vxtxr_ndesc;
		txs->comp_ring = txq->vxtxq_comp_ring.vxcr_dma.dma_paddr;
		txs->comp_ring_len = txq->vxtxq_comp_ring.vxcr_ndesc;
		txs->driver_data = vtophys(txq);
		txs->driver_data_len = sizeof(struct vmxnet3_txqueue);
	}

	/* Rx queues */
	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		rxq = &sc->vmx_queue[i].vxq_rxqueue;
		rxs = rxq->vxrxq_rs;

		rxs->cmd_ring[0] = rxq->vxrxq_cmd_ring[0].vxrxr_dma.dma_paddr;
		rxs->cmd_ring_len[0] = rxq->vxrxq_cmd_ring[0].vxrxr_ndesc;
		rxs->cmd_ring[1] = rxq->vxrxq_cmd_ring[1].vxrxr_dma.dma_paddr;
		rxs->cmd_ring_len[1] = rxq->vxrxq_cmd_ring[1].vxrxr_ndesc;
		rxs->comp_ring = rxq->vxrxq_comp_ring.vxcr_dma.dma_paddr;
		rxs->comp_ring_len = rxq->vxrxq_comp_ring.vxcr_ndesc;
		rxs->driver_data = vtophys(rxq);
		rxs->driver_data_len = sizeof(struct vmxnet3_rxqueue);
	}
}

static void
vmxnet3_reinit_rss_shared_data(struct vmxnet3_softc *sc)
{
	/*
	 * Use the same key as the Linux driver until FreeBSD can do
	 * RSS (presumably Toeplitz) in software.
	 */
	static const uint8_t rss_key[UPT1_RSS_MAX_KEY_SIZE] = {
	    0x3b, 0x56, 0xd1, 0x56, 0x13, 0x4a, 0xe7, 0xac,
	    0xe8, 0x79, 0x09, 0x75, 0xe8, 0x65, 0x79, 0x28,
	    0x35, 0x12, 0xb9, 0x56, 0x7c, 0x76, 0x4b, 0x70,
	    0xd8, 0x56, 0xa3, 0x18, 0x9b, 0x0a, 0xee, 0xf3,
	    0x96, 0xa6, 0x9f, 0x8f, 0x9e, 0x8c, 0x90, 0xc9,
	};

	struct vmxnet3_rss_shared *rss;
	int i;

	rss = sc->vmx_rss;

	rss->hash_type =
	    UPT1_RSS_HASH_TYPE_IPV4 | UPT1_RSS_HASH_TYPE_TCP_IPV4 |
	    UPT1_RSS_HASH_TYPE_IPV6 | UPT1_RSS_HASH_TYPE_TCP_IPV6;
	rss->hash_func = UPT1_RSS_HASH_FUNC_TOEPLITZ;
	rss->hash_key_size = UPT1_RSS_MAX_KEY_SIZE;
	rss->ind_table_size = UPT1_RSS_MAX_IND_TABLE_SIZE;
	memcpy(rss->hash_key, rss_key, UPT1_RSS_MAX_KEY_SIZE);

	for (i = 0; i < UPT1_RSS_MAX_IND_TABLE_SIZE; i++)
		rss->ind_table[i] = i % sc->vmx_nrxqueues;
}

static void
vmxnet3_reinit_shared_data(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	struct vmxnet3_driver_shared *ds;

	ifp = &sc->vmx_ethercom.ec_if;
	ds = sc->vmx_ds;

	ds->mtu = ifp->if_mtu;
	ds->ntxqueue = sc->vmx_ntxqueues;
	ds->nrxqueue = sc->vmx_nrxqueues;

	ds->upt_features = 0;
	if (ifp->if_capenable &
	    (IFCAP_CSUM_IPv4_Rx | IFCAP_CSUM_TCPv4_Rx | IFCAP_CSUM_UDPv4_Rx |
	    IFCAP_CSUM_TCPv6_Rx | IFCAP_CSUM_UDPv6_Rx))
		ds->upt_features |= UPT1_F_CSUM;
	if (sc->vmx_ethercom.ec_capenable & ETHERCAP_VLAN_HWTAGGING)
		ds->upt_features |= UPT1_F_VLAN;

	if (sc->vmx_flags & VMXNET3_FLAG_RSS) {
		ds->upt_features |= UPT1_F_RSS;
		vmxnet3_reinit_rss_shared_data(sc);
	}

	vmxnet3_write_bar1(sc, VMXNET3_BAR1_DSL, sc->vmx_ds_dma.dma_paddr);
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_DSH,
	    (uint64_t) sc->vmx_ds_dma.dma_paddr >> 32);
}

static int
vmxnet3_alloc_data(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_alloc_shared_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_queue_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_mcast_table(sc);
	if (error)
		return (error);

	vmxnet3_init_shared_data(sc);

	return (0);
}

static void
vmxnet3_free_data(struct vmxnet3_softc *sc)
{

	vmxnet3_free_mcast_table(sc);
	vmxnet3_free_queue_data(sc);
	vmxnet3_free_shared_data(sc);
}

static int
vmxnet3_setup_interface(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp = &sc->vmx_ethercom.ec_if;

	vmxnet3_get_lladdr(sc);
	aprint_normal_dev(sc->vmx_dev, "Ethernet address %s\n",
	    ether_sprintf(sc->vmx_lladdr));
	vmxnet3_set_lladdr(sc);

	strlcpy(ifp->if_xname, device_xname(sc->vmx_dev), IFNAMSIZ);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX;
	ifp->if_extflags = IFEF_MPSAFE;
	ifp->if_ioctl = vmxnet3_ioctl;
	ifp->if_start = vmxnet3_start;
	ifp->if_transmit = vmxnet3_transmit;
	ifp->if_watchdog = NULL;
	ifp->if_init = vmxnet3_init;
	ifp->if_stop = vmxnet3_stop;
	sc->vmx_ethercom.ec_if.if_capabilities |=IFCAP_CSUM_IPv4_Rx |
		    IFCAP_CSUM_TCPv4_Tx | IFCAP_CSUM_TCPv4_Rx |
		    IFCAP_CSUM_UDPv4_Tx | IFCAP_CSUM_UDPv4_Rx |
		    IFCAP_CSUM_TCPv6_Tx | IFCAP_CSUM_TCPv6_Rx |
		    IFCAP_CSUM_UDPv6_Tx | IFCAP_CSUM_UDPv6_Rx;

	ifp->if_capenable = ifp->if_capabilities;

	sc->vmx_ethercom.ec_if.if_capabilities |= IFCAP_TSOv4 | IFCAP_TSOv6;

	sc->vmx_ethercom.ec_capabilities |=
	    ETHERCAP_VLAN_MTU | ETHERCAP_VLAN_HWTAGGING | ETHERCAP_JUMBO_MTU;
	sc->vmx_ethercom.ec_capenable |= ETHERCAP_VLAN_HWTAGGING;

	IFQ_SET_MAXLEN(&ifp->if_snd, sc->vmx_ntxdescs);
	IFQ_SET_READY(&ifp->if_snd);

	/* Initialize ifmedia structures. */
	sc->vmx_ethercom.ec_ifmedia = &sc->vmx_media;
	ifmedia_init_with_lock(&sc->vmx_media, IFM_IMASK, vmxnet3_ifmedia_change,
	    vmxnet3_ifmedia_status, sc->vmx_mtx);
	ifmedia_add(&sc->vmx_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_add(&sc->vmx_media, IFM_ETHER | IFM_10G_T | IFM_FDX, 0, NULL);
	ifmedia_add(&sc->vmx_media, IFM_ETHER | IFM_10G_T, 0, NULL);
	ifmedia_add(&sc->vmx_media, IFM_ETHER | IFM_1000_T | IFM_FDX, 0, NULL);
	ifmedia_add(&sc->vmx_media, IFM_ETHER | IFM_1000_T, 0, NULL);
	ifmedia_set(&sc->vmx_media, IFM_ETHER | IFM_AUTO);

	if_attach(ifp);
	if_deferred_start_init(ifp, NULL);
	ether_ifattach(ifp, sc->vmx_lladdr);
	ether_set_ifflags_cb(&sc->vmx_ethercom, vmxnet3_ifflags_cb);
	vmxnet3_cmd_link_status(ifp);

	/* should set before setting interrupts */
	sc->vmx_rx_intr_process_limit = VMXNET3_RX_INTR_PROCESS_LIMIT;
	sc->vmx_rx_process_limit = VMXNET3_RX_PROCESS_LIMIT;
	sc->vmx_tx_intr_process_limit = VMXNET3_TX_INTR_PROCESS_LIMIT;
	sc->vmx_tx_process_limit = VMXNET3_TX_PROCESS_LIMIT;

	return (0);
}

static int
vmxnet3_setup_sysctl(struct vmxnet3_softc *sc)
{
	const char *devname;
	struct sysctllog **log;
	const struct sysctlnode *rnode, *rxnode, *txnode;
	int error;

	log = &sc->vmx_sysctllog;
	devname = device_xname(sc->vmx_dev);

	error = sysctl_createv(log, 0, NULL, &rnode,
	    0, CTLTYPE_NODE, devname,
	    SYSCTL_DESCR("vmxnet3 information and settings"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL);
	if (error)
		goto out;
	error = sysctl_createv(log, 0, &rnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_BOOL, "txrx_workqueue",
	    SYSCTL_DESCR("Use workqueue for packet processing"),
	    NULL, 0, &sc->vmx_txrx_workqueue, 0, CTL_CREATE, CTL_EOL);
	if (error)
		goto out;

	error = sysctl_createv(log, 0, &rnode, &rxnode,
	    0, CTLTYPE_NODE, "rx",
	    SYSCTL_DESCR("vmxnet3 information and settings for Rx"),
	    NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	if (error)
		goto out;
	error = sysctl_createv(log, 0, &rxnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "intr_process_limit",
	    SYSCTL_DESCR("max number of Rx packets to process for interrupt processing"),
	    NULL, 0, &sc->vmx_rx_intr_process_limit, 0, CTL_CREATE, CTL_EOL);
	if (error)
		goto out;
	error = sysctl_createv(log, 0, &rxnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "process_limit",
	    SYSCTL_DESCR("max number of Rx packets to process for deferred processing"),
	    NULL, 0, &sc->vmx_rx_process_limit, 0, CTL_CREATE, CTL_EOL);
	if (error)
		goto out;

	error = sysctl_createv(log, 0, &rnode, &txnode,
	    0, CTLTYPE_NODE, "tx",
	    SYSCTL_DESCR("vmxnet3 information and settings for Tx"),
	    NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	if (error)
		goto out;
	error = sysctl_createv(log, 0, &txnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "intr_process_limit",
	    SYSCTL_DESCR("max number of Tx packets to process for interrupt processing"),
	    NULL, 0, &sc->vmx_tx_intr_process_limit, 0, CTL_CREATE, CTL_EOL);
	if (error)
		goto out;
	error = sysctl_createv(log, 0, &txnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "process_limit",
	    SYSCTL_DESCR("max number of Tx packets to process for deferred processing"),
	    NULL, 0, &sc->vmx_tx_process_limit, 0, CTL_CREATE, CTL_EOL);

out:
	if (error) {
		aprint_error_dev(sc->vmx_dev,
		    "unable to create sysctl node\n");
		sysctl_teardown(log);
	}
	return error;
}

static int
vmxnet3_setup_stats(struct vmxnet3_softc *sc)
{
	struct vmxnet3_queue *vmxq;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_rxqueue *rxq;
	int i;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		vmxq = &sc->vmx_queue[i];
		txq = &vmxq->vxq_txqueue;
		evcnt_attach_dynamic(&txq->vxtxq_intr, EVCNT_TYPE_INTR,
		    NULL, txq->vxtxq_name, "Interrupt on queue");
		evcnt_attach_dynamic(&txq->vxtxq_defer, EVCNT_TYPE_MISC,
		    NULL, txq->vxtxq_name, "Handled queue in softint/workqueue");
		evcnt_attach_dynamic(&txq->vxtxq_deferreq, EVCNT_TYPE_MISC,
		    NULL, txq->vxtxq_name, "Requested in softint/workqueue");
		evcnt_attach_dynamic(&txq->vxtxq_pcqdrop, EVCNT_TYPE_MISC,
		    NULL, txq->vxtxq_name, "Dropped in pcq");
		evcnt_attach_dynamic(&txq->vxtxq_transmitdef, EVCNT_TYPE_MISC,
		    NULL, txq->vxtxq_name, "Deferred transmit");
		evcnt_attach_dynamic(&txq->vxtxq_watchdogto, EVCNT_TYPE_MISC,
		    NULL, txq->vxtxq_name, "Watchdog timeout");
		evcnt_attach_dynamic(&txq->vxtxq_defragged, EVCNT_TYPE_MISC,
		    NULL, txq->vxtxq_name, "m_defrag successed");
		evcnt_attach_dynamic(&txq->vxtxq_defrag_failed, EVCNT_TYPE_MISC,
		    NULL, txq->vxtxq_name, "m_defrag failed");
	}

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		vmxq = &sc->vmx_queue[i];
		rxq = &vmxq->vxq_rxqueue;
		evcnt_attach_dynamic(&rxq->vxrxq_intr, EVCNT_TYPE_INTR,
		    NULL, rxq->vxrxq_name, "Interrupt on queue");
		evcnt_attach_dynamic(&rxq->vxrxq_defer, EVCNT_TYPE_MISC,
		    NULL, rxq->vxrxq_name, "Handled queue in softint/workqueue");
		evcnt_attach_dynamic(&rxq->vxrxq_deferreq, EVCNT_TYPE_MISC,
		    NULL, rxq->vxrxq_name, "Requested in softint/workqueue");
		evcnt_attach_dynamic(&rxq->vxrxq_mgetcl_failed, EVCNT_TYPE_MISC,
		    NULL, rxq->vxrxq_name, "MCLGET failed");
		evcnt_attach_dynamic(&rxq->vxrxq_mbuf_load_failed, EVCNT_TYPE_MISC,
		    NULL, rxq->vxrxq_name, "bus_dmamap_load_mbuf failed");
	}

	evcnt_attach_dynamic(&sc->vmx_event_intr, EVCNT_TYPE_INTR,
	    NULL, device_xname(sc->vmx_dev), "Interrupt for other events");
	evcnt_attach_dynamic(&sc->vmx_event_link, EVCNT_TYPE_MISC,
	    NULL, device_xname(sc->vmx_dev), "Link status event");
	evcnt_attach_dynamic(&sc->vmx_event_txqerror, EVCNT_TYPE_MISC,
	    NULL, device_xname(sc->vmx_dev), "Tx queue error event");
	evcnt_attach_dynamic(&sc->vmx_event_rxqerror, EVCNT_TYPE_MISC,
	    NULL, device_xname(sc->vmx_dev), "Rx queue error event");
	evcnt_attach_dynamic(&sc->vmx_event_dic, EVCNT_TYPE_MISC,
	    NULL, device_xname(sc->vmx_dev), "Device impl change event");
	evcnt_attach_dynamic(&sc->vmx_event_debug, EVCNT_TYPE_MISC,
	    NULL, device_xname(sc->vmx_dev), "Debug event");

	return 0;
}

static void
vmxnet3_teardown_stats(struct vmxnet3_softc *sc)
{
	struct vmxnet3_queue *vmxq;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_rxqueue *rxq;
	int i;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		vmxq = &sc->vmx_queue[i];
		txq = &vmxq->vxq_txqueue;
		evcnt_detach(&txq->vxtxq_intr);
		evcnt_detach(&txq->vxtxq_defer);
		evcnt_detach(&txq->vxtxq_deferreq);
		evcnt_detach(&txq->vxtxq_pcqdrop);
		evcnt_detach(&txq->vxtxq_transmitdef);
		evcnt_detach(&txq->vxtxq_watchdogto);
		evcnt_detach(&txq->vxtxq_defragged);
		evcnt_detach(&txq->vxtxq_defrag_failed);
	}

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		vmxq = &sc->vmx_queue[i];
		rxq = &vmxq->vxq_rxqueue;
		evcnt_detach(&rxq->vxrxq_intr);
		evcnt_detach(&rxq->vxrxq_defer);
		evcnt_detach(&rxq->vxrxq_deferreq);
		evcnt_detach(&rxq->vxrxq_mgetcl_failed);
		evcnt_detach(&rxq->vxrxq_mbuf_load_failed);
	}

	evcnt_detach(&sc->vmx_event_intr);
	evcnt_detach(&sc->vmx_event_link);
	evcnt_detach(&sc->vmx_event_txqerror);
	evcnt_detach(&sc->vmx_event_rxqerror);
	evcnt_detach(&sc->vmx_event_dic);
	evcnt_detach(&sc->vmx_event_debug);
}

static void
vmxnet3_evintr(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_txq_shared *ts;
	struct vmxnet3_rxq_shared *rs;
	uint32_t event;
	int reset;

	dev = sc->vmx_dev;
	reset = 0;

	VMXNET3_CORE_LOCK(sc);

	/* Clear events. */
	event = sc->vmx_ds->event;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_EVENT, event);

	if (event & VMXNET3_EVENT_LINK) {
		sc->vmx_event_link.ev_count++;
		vmxnet3_if_link_status(sc);
		if (sc->vmx_link_active != 0)
			if_schedule_deferred_start(&sc->vmx_ethercom.ec_if);
	}

	if (event & (VMXNET3_EVENT_TQERROR | VMXNET3_EVENT_RQERROR)) {
		if (event & VMXNET3_EVENT_TQERROR)
			sc->vmx_event_txqerror.ev_count++;
		if (event & VMXNET3_EVENT_RQERROR)
			sc->vmx_event_rxqerror.ev_count++;

		reset = 1;
		vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_STATUS);
		ts = sc->vmx_queue[0].vxq_txqueue.vxtxq_ts;
		if (ts->stopped != 0)
			device_printf(dev, "Tx queue error %#x\n", ts->error);
		rs = sc->vmx_queue[0].vxq_rxqueue.vxrxq_rs;
		if (rs->stopped != 0)
			device_printf(dev, "Rx queue error %#x\n", rs->error);
		device_printf(dev, "Rx/Tx queue error event ... resetting\n");
	}

	if (event & VMXNET3_EVENT_DIC) {
		sc->vmx_event_dic.ev_count++;
		device_printf(dev, "device implementation change event\n");
	}
	if (event & VMXNET3_EVENT_DEBUG) {
		sc->vmx_event_debug.ev_count++;
		device_printf(dev, "debug event\n");
	}

	if (reset != 0)
		vmxnet3_init_locked(sc);

	VMXNET3_CORE_UNLOCK(sc);
}

static bool
vmxnet3_txq_eof(struct vmxnet3_txqueue *txq, u_int limit)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	struct vmxnet3_txcompdesc *txcd;
	struct vmxnet3_txbuf *txb;
	struct ifnet *ifp;
	struct mbuf *m;
	u_int sop;
	bool more = false;

	sc = txq->vxtxq_sc;
	txr = &txq->vxtxq_cmd_ring;
	txc = &txq->vxtxq_comp_ring;
	ifp = &sc->vmx_ethercom.ec_if;

	VMXNET3_TXQ_LOCK_ASSERT(txq);

	net_stat_ref_t nsr = IF_STAT_GETREF(ifp);
	for (;;) {
		if (limit-- == 0) {
			more = true;
			break;
		}

		txcd = &txc->vxcr_u.txcd[txc->vxcr_next];
		if (txcd->gen != txc->vxcr_gen)
			break;
		vmxnet3_barrier(sc, VMXNET3_BARRIER_RD);

		if (++txc->vxcr_next == txc->vxcr_ndesc) {
			txc->vxcr_next = 0;
			txc->vxcr_gen ^= 1;
		}

		sop = txr->vxtxr_next;
		txb = &txr->vxtxr_txbuf[sop];

		if ((m = txb->vtxb_m) != NULL) {
			bus_dmamap_sync(sc->vmx_dmat, txb->vtxb_dmamap,
			    0, txb->vtxb_dmamap->dm_mapsize,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->vmx_dmat, txb->vtxb_dmamap);

			if_statinc_ref(nsr, if_opackets);
			if_statadd_ref(nsr, if_obytes, m->m_pkthdr.len);
			if (m->m_flags & M_MCAST)
				if_statinc_ref(nsr, if_omcasts);

			m_freem(m);
			txb->vtxb_m = NULL;
		}

		txr->vxtxr_next = (txcd->eop_idx + 1) % txr->vxtxr_ndesc;
	}
	IF_STAT_PUTREF(ifp);

	if (txr->vxtxr_head == txr->vxtxr_next)
		txq->vxtxq_watchdog = 0;

	return more;
}

static int
vmxnet3_newbuf(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq,
    struct vmxnet3_rxring *rxr)
{
	struct mbuf *m;
	struct vmxnet3_rxdesc *rxd;
	struct vmxnet3_rxbuf *rxb;
	bus_dma_tag_t tag;
	bus_dmamap_t dmap;
	int idx, btype, error;

	tag = sc->vmx_dmat;
	dmap = rxr->vxrxr_spare_dmap;
	idx = rxr->vxrxr_fill;
	rxd = &rxr->vxrxr_rxd[idx];
	rxb = &rxr->vxrxr_rxbuf[idx];

	/* Don't allocate buffers for ring 2 for now. */
	if (rxr->vxrxr_rid != 0)
		return -1;
	btype = VMXNET3_BTYPE_HEAD;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return (ENOBUFS);

	MCLGET(m, M_DONTWAIT);
	if ((m->m_flags & M_EXT) == 0) {
		rxq->vxrxq_mgetcl_failed.ev_count++;
		m_freem(m);
		return (ENOBUFS);
	}

	m->m_pkthdr.len = m->m_len = JUMBO_LEN;
	m_adj(m, ETHER_ALIGN);

	error = bus_dmamap_load_mbuf(sc->vmx_dmat, dmap, m, BUS_DMA_NOWAIT);
	if (error) {
		m_freem(m);
		rxq->vxrxq_mbuf_load_failed.ev_count++;
		return (error);
	}

	if (rxb->vrxb_m != NULL) {
		bus_dmamap_sync(tag, rxb->vrxb_dmamap,
		    0, rxb->vrxb_dmamap->dm_mapsize,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(tag, rxb->vrxb_dmamap);
	}

	rxr->vxrxr_spare_dmap = rxb->vrxb_dmamap;
	rxb->vrxb_dmamap = dmap;
	rxb->vrxb_m = m;

	rxd->addr = DMAADDR(dmap);
	rxd->len = m->m_pkthdr.len;
	rxd->btype = btype;
	rxd->gen = rxr->vxrxr_gen;

	vmxnet3_rxr_increment_fill(rxr);
	return (0);
}

static void
vmxnet3_rxq_eof_discard(struct vmxnet3_rxqueue *rxq,
    struct vmxnet3_rxring *rxr, int idx)
{
	struct vmxnet3_rxdesc *rxd;

	rxd = &rxr->vxrxr_rxd[idx];
	rxd->gen = rxr->vxrxr_gen;
	vmxnet3_rxr_increment_fill(rxr);
}

static void
vmxnet3_rxq_discard_chain(struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	struct vmxnet3_rxcompdesc *rxcd;
	int idx, eof;

	sc = rxq->vxrxq_sc;
	rxc = &rxq->vxrxq_comp_ring;

	do {
		rxcd = &rxc->vxcr_u.rxcd[rxc->vxcr_next];
		if (rxcd->gen != rxc->vxcr_gen)
			break;		/* Not expected. */
		vmxnet3_barrier(sc, VMXNET3_BARRIER_RD);

		if (++rxc->vxcr_next == rxc->vxcr_ndesc) {
			rxc->vxcr_next = 0;
			rxc->vxcr_gen ^= 1;
		}

		idx = rxcd->rxd_idx;
		eof = rxcd->eop;
		if (rxcd->qid < sc->vmx_nrxqueues)
			rxr = &rxq->vxrxq_cmd_ring[0];
		else
			rxr = &rxq->vxrxq_cmd_ring[1];
		vmxnet3_rxq_eof_discard(rxq, rxr, idx);
	} while (!eof);
}

static void
vmxnet3_rx_csum(struct vmxnet3_rxcompdesc *rxcd, struct mbuf *m)
{
	if (rxcd->no_csum)
		return;

	if (rxcd->ipv4) {
		m->m_pkthdr.csum_flags |= M_CSUM_IPv4;
		if (rxcd->ipcsum_ok == 0)
			m->m_pkthdr.csum_flags |= M_CSUM_IPv4_BAD;
	}

	if (rxcd->fragment)
		return;

	if (rxcd->tcp) {
		m->m_pkthdr.csum_flags |=
		    rxcd->ipv4 ? M_CSUM_TCPv4 : M_CSUM_TCPv6;
		if ((rxcd->csum_ok) == 0)
			m->m_pkthdr.csum_flags |= M_CSUM_TCP_UDP_BAD;
	}

	if (rxcd->udp) {
		m->m_pkthdr.csum_flags |=
		    rxcd->ipv4 ? M_CSUM_UDPv4 : M_CSUM_UDPv6 ;
		if ((rxcd->csum_ok) == 0)
			m->m_pkthdr.csum_flags |= M_CSUM_TCP_UDP_BAD;
	}
}

static void
vmxnet3_rxq_input(struct vmxnet3_rxqueue *rxq,
    struct vmxnet3_rxcompdesc *rxcd, struct mbuf *m)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;

	sc = rxq->vxrxq_sc;
	ifp = &sc->vmx_ethercom.ec_if;

	if (rxcd->error) {
		if_statinc(ifp, if_ierrors);
		m_freem(m);
		return;
	}

	if (!rxcd->no_csum)
		vmxnet3_rx_csum(rxcd, m);
	if (rxcd->vlan)
		vlan_set_tag(m, rxcd->vtag);

	net_stat_ref_t nsr = IF_STAT_GETREF(ifp);
	if_statinc_ref(nsr, if_ipackets);
	if_statadd_ref(nsr, if_ibytes, m->m_pkthdr.len);
	IF_STAT_PUTREF(ifp);

	if_percpuq_enqueue(ifp->if_percpuq, m);
}

static bool
vmxnet3_rxq_eof(struct vmxnet3_rxqueue *rxq, u_int limit)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	struct vmxnet3_rxdesc *rxd __diagused;
	struct vmxnet3_rxcompdesc *rxcd;
	struct mbuf *m, *m_head, *m_tail;
	u_int idx, length;
	bool more = false;

	sc = rxq->vxrxq_sc;
	ifp = &sc->vmx_ethercom.ec_if;
	rxc = &rxq->vxrxq_comp_ring;

	VMXNET3_RXQ_LOCK_ASSERT(rxq);

	if ((ifp->if_flags & IFF_RUNNING) == 0)
		return more;

	m_head = rxq->vxrxq_mhead;
	rxq->vxrxq_mhead = NULL;
	m_tail = rxq->vxrxq_mtail;
	rxq->vxrxq_mtail = NULL;
	KASSERT(m_head == NULL || m_tail != NULL);

	for (;;) {
		if (limit-- == 0) {
			more = true;
			break;
		}

		rxcd = &rxc->vxcr_u.rxcd[rxc->vxcr_next];
		if (rxcd->gen != rxc->vxcr_gen) {
			rxq->vxrxq_mhead = m_head;
			rxq->vxrxq_mtail = m_tail;
			break;
		}
		vmxnet3_barrier(sc, VMXNET3_BARRIER_RD);

		if (++rxc->vxcr_next == rxc->vxcr_ndesc) {
			rxc->vxcr_next = 0;
			rxc->vxcr_gen ^= 1;
		}

		idx = rxcd->rxd_idx;
		length = rxcd->len;
		if (rxcd->qid < sc->vmx_nrxqueues)
			rxr = &rxq->vxrxq_cmd_ring[0];
		else
			rxr = &rxq->vxrxq_cmd_ring[1];
		rxd = &rxr->vxrxr_rxd[idx];

		m = rxr->vxrxr_rxbuf[idx].vrxb_m;
		KASSERT(m != NULL);

		/*
		 * The host may skip descriptors. We detect this when this
		 * descriptor does not match the previous fill index. Catch
		 * up with the host now.
		 */
		if (__predict_false(rxr->vxrxr_fill != idx)) {
			while (rxr->vxrxr_fill != idx) {
				rxr->vxrxr_rxd[rxr->vxrxr_fill].gen =
				    rxr->vxrxr_gen;
				vmxnet3_rxr_increment_fill(rxr);
			}
		}

		if (rxcd->sop) {
			/* start of frame w/o head buffer */
			KASSERT(rxd->btype == VMXNET3_BTYPE_HEAD);
			/* start of frame not in ring 0 */
			KASSERT(rxr == &rxq->vxrxq_cmd_ring[0]);
			/* duplicate start of frame? */
			KASSERT(m_head == NULL);

			if (length == 0) {
				/* Just ignore this descriptor. */
				vmxnet3_rxq_eof_discard(rxq, rxr, idx);
				goto nextp;
			}

			if (vmxnet3_newbuf(sc, rxq, rxr) != 0) {
				if_statinc(ifp, if_iqdrops);
				vmxnet3_rxq_eof_discard(rxq, rxr, idx);
				if (!rxcd->eop)
					vmxnet3_rxq_discard_chain(rxq);
				goto nextp;
			}

			m_set_rcvif(m, ifp);
			m->m_pkthdr.len = m->m_len = length;
			m->m_pkthdr.csum_flags = 0;
			m_head = m_tail = m;

		} else {
			/* non start of frame w/o body buffer */
			KASSERT(rxd->btype == VMXNET3_BTYPE_BODY);
			/* frame not started? */
			KASSERT(m_head != NULL);

			if (vmxnet3_newbuf(sc, rxq, rxr) != 0) {
				if_statinc(ifp, if_iqdrops);
				vmxnet3_rxq_eof_discard(rxq, rxr, idx);
				if (!rxcd->eop)
					vmxnet3_rxq_discard_chain(rxq);
				m_freem(m_head);
				m_head = m_tail = NULL;
				goto nextp;
			}

			m->m_len = length;
			m_head->m_pkthdr.len += length;
			m_tail->m_next = m;
			m_tail = m;
		}

		if (rxcd->eop) {
			vmxnet3_rxq_input(rxq, rxcd, m_head);
			m_head = m_tail = NULL;

			/* Must recheck after dropping the Rx lock. */
			if ((ifp->if_flags & IFF_RUNNING) == 0)
				break;
		}

nextp:
		if (__predict_false(rxq->vxrxq_rs->update_rxhead)) {
			int qid = rxcd->qid;
			bus_size_t r;

			idx = (idx + 1) % rxr->vxrxr_ndesc;
			if (qid >= sc->vmx_nrxqueues) {
				qid -= sc->vmx_nrxqueues;
				r = VMXNET3_BAR0_RXH2(qid);
			} else
				r = VMXNET3_BAR0_RXH1(qid);
			vmxnet3_write_bar0(sc, r, idx);
		}
	}

	return more;
}

static inline void
vmxnet3_sched_handle_queue(struct vmxnet3_softc *sc, struct vmxnet3_queue *vmxq)
{

	if (vmxq->vxq_workqueue) {
		/*
		 * When this function is called, "vmxq" is owned by one CPU.
		 * so, atomic operation is not required here.
		 */
		if (!vmxq->vxq_wq_enqueued) {
			vmxq->vxq_wq_enqueued = true;
			workqueue_enqueue(sc->vmx_queue_wq,
			    &vmxq->vxq_wq_cookie, curcpu());
		}
	} else {
		softint_schedule(vmxq->vxq_si);
	}
}

static int
vmxnet3_legacy_intr(void *xsc)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_txqueue *txq;
	u_int txlimit, rxlimit;
	bool txmore, rxmore;

	sc = xsc;
	rxq = &sc->vmx_queue[0].vxq_rxqueue;
	txq = &sc->vmx_queue[0].vxq_txqueue;
	txlimit = sc->vmx_tx_intr_process_limit;
	rxlimit = sc->vmx_rx_intr_process_limit;

	if (sc->vmx_intr_type == VMXNET3_IT_LEGACY) {
		if (vmxnet3_read_bar1(sc, VMXNET3_BAR1_INTR) == 0)
			return (0);
	}
	if (sc->vmx_intr_mask_mode == VMXNET3_IMM_ACTIVE)
		vmxnet3_disable_all_intrs(sc);

	if (sc->vmx_ds->event != 0)
		vmxnet3_evintr(sc);

	VMXNET3_RXQ_LOCK(rxq);
	rxmore = vmxnet3_rxq_eof(rxq, rxlimit);
	VMXNET3_RXQ_UNLOCK(rxq);

	VMXNET3_TXQ_LOCK(txq);
	txmore = vmxnet3_txq_eof(txq, txlimit);
	VMXNET3_TXQ_UNLOCK(txq);

	if (txmore || rxmore) {
		vmxnet3_sched_handle_queue(sc, &sc->vmx_queue[0]);
	} else {
		if_schedule_deferred_start(&sc->vmx_ethercom.ec_if);
		vmxnet3_enable_all_intrs(sc);
	}
	return (1);
}

static int
vmxnet3_txrxq_intr(void *xvmxq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_queue *vmxq;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_rxqueue *rxq;
	u_int txlimit, rxlimit;
	bool txmore, rxmore;

	vmxq = xvmxq;
	txq = &vmxq->vxq_txqueue;
	rxq = &vmxq->vxq_rxqueue;
	sc = txq->vxtxq_sc;
	txlimit = sc->vmx_tx_intr_process_limit;
	rxlimit = sc->vmx_rx_intr_process_limit;
	vmxq->vxq_workqueue = sc->vmx_txrx_workqueue;

	if (sc->vmx_intr_mask_mode == VMXNET3_IMM_ACTIVE)
		vmxnet3_disable_intr(sc, vmxq->vxq_intr_idx);

	VMXNET3_TXQ_LOCK(txq);
	txq->vxtxq_intr.ev_count++;
	txmore = vmxnet3_txq_eof(txq, txlimit);
	VMXNET3_TXQ_UNLOCK(txq);

	VMXNET3_RXQ_LOCK(rxq);
	rxq->vxrxq_intr.ev_count++;
	rxmore = vmxnet3_rxq_eof(rxq, rxlimit);
	VMXNET3_RXQ_UNLOCK(rxq);

	if (txmore || rxmore) {
		vmxnet3_sched_handle_queue(sc, vmxq);
	} else {
		/* for ALTQ */
		if (vmxq->vxq_id == 0)
			if_schedule_deferred_start(&sc->vmx_ethercom.ec_if);
		softint_schedule(txq->vxtxq_si);

		vmxnet3_enable_intr(sc, vmxq->vxq_intr_idx);
	}

	return (1);
}

static void
vmxnet3_handle_queue(void *xvmxq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_queue *vmxq;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_rxqueue *rxq;
	u_int txlimit, rxlimit;
	bool txmore, rxmore;

	vmxq = xvmxq;
	txq = &vmxq->vxq_txqueue;
	rxq = &vmxq->vxq_rxqueue;
	sc = txq->vxtxq_sc;
	txlimit = sc->vmx_tx_process_limit;
	rxlimit = sc->vmx_rx_process_limit;

	VMXNET3_TXQ_LOCK(txq);
	txq->vxtxq_defer.ev_count++;
	txmore = vmxnet3_txq_eof(txq, txlimit);
	if (txmore)
		txq->vxtxq_deferreq.ev_count++;
	/* for ALTQ */
	if (vmxq->vxq_id == 0)
		if_schedule_deferred_start(&sc->vmx_ethercom.ec_if);
	softint_schedule(txq->vxtxq_si);
	VMXNET3_TXQ_UNLOCK(txq);

	VMXNET3_RXQ_LOCK(rxq);
	rxq->vxrxq_defer.ev_count++;
	rxmore = vmxnet3_rxq_eof(rxq, rxlimit);
	if (rxmore)
		rxq->vxrxq_deferreq.ev_count++;
	VMXNET3_RXQ_UNLOCK(rxq);

	if (txmore || rxmore)
		vmxnet3_sched_handle_queue(sc, vmxq);
	else
		vmxnet3_enable_intr(sc, vmxq->vxq_intr_idx);
}

static void
vmxnet3_handle_queue_work(struct work *wk, void *context)
{
	struct vmxnet3_queue *vmxq;

	vmxq = container_of(wk, struct vmxnet3_queue, vxq_wq_cookie);
	vmxq->vxq_wq_enqueued = false;
	vmxnet3_handle_queue(vmxq);
}

static int
vmxnet3_event_intr(void *xsc)
{
	struct vmxnet3_softc *sc;

	sc = xsc;

	if (sc->vmx_intr_mask_mode == VMXNET3_IMM_ACTIVE)
		vmxnet3_disable_intr(sc, sc->vmx_event_intr_idx);

	sc->vmx_event_intr.ev_count++;

	if (sc->vmx_ds->event != 0)
		vmxnet3_evintr(sc);

	vmxnet3_enable_intr(sc, sc->vmx_event_intr_idx);

	return (1);
}

static void
vmxnet3_txstop(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;
	struct vmxnet3_txbuf *txb;
	u_int i;

	txr = &txq->vxtxq_cmd_ring;

	for (i = 0; i < txr->vxtxr_ndesc; i++) {
		txb = &txr->vxtxr_txbuf[i];

		if (txb->vtxb_m == NULL)
			continue;

		bus_dmamap_sync(sc->vmx_dmat, txb->vtxb_dmamap,
		    0, txb->vtxb_dmamap->dm_mapsize,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->vmx_dmat, txb->vtxb_dmamap);
		m_freem(txb->vtxb_m);
		txb->vtxb_m = NULL;
	}
}

static void
vmxnet3_rxstop(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_rxbuf *rxb;
	u_int i, j;

	if (rxq->vxrxq_mhead != NULL) {
		m_freem(rxq->vxrxq_mhead);
		rxq->vxrxq_mhead = NULL;
		rxq->vxrxq_mtail = NULL;
	}

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];

		for (j = 0; j < rxr->vxrxr_ndesc; j++) {
			rxb = &rxr->vxrxr_rxbuf[j];

			if (rxb->vrxb_m == NULL)
				continue;

			bus_dmamap_sync(sc->vmx_dmat, rxb->vrxb_dmamap,
			    0, rxb->vrxb_dmamap->dm_mapsize,
			    BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(sc->vmx_dmat, rxb->vrxb_dmamap);
			m_freem(rxb->vrxb_m);
			rxb->vrxb_m = NULL;
		}
	}
}

static void
vmxnet3_stop_rendezvous(struct vmxnet3_softc *sc)
{
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_queue *vmxq;
	int i;

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		rxq = &sc->vmx_queue[i].vxq_rxqueue;
		VMXNET3_RXQ_LOCK(rxq);
		VMXNET3_RXQ_UNLOCK(rxq);
	}
	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_queue[i].vxq_txqueue;
		VMXNET3_TXQ_LOCK(txq);
		VMXNET3_TXQ_UNLOCK(txq);
	}
	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		vmxq = &sc->vmx_queue[i];
		workqueue_wait(sc->vmx_queue_wq, &vmxq->vxq_wq_cookie);
	}
}

static void
vmxnet3_stop_locked(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	int q;

	ifp = &sc->vmx_ethercom.ec_if;
	VMXNET3_CORE_LOCK_ASSERT(sc);

	ifp->if_flags &= ~IFF_RUNNING;
	sc->vmx_link_active = 0;
	callout_stop(&sc->vmx_tick);

	/* Disable interrupts. */
	vmxnet3_disable_all_intrs(sc);
	vmxnet3_write_cmd(sc, VMXNET3_CMD_DISABLE);

	vmxnet3_stop_rendezvous(sc);

	for (q = 0; q < sc->vmx_ntxqueues; q++)
		vmxnet3_txstop(sc, &sc->vmx_queue[q].vxq_txqueue);
	for (q = 0; q < sc->vmx_nrxqueues; q++)
		vmxnet3_rxstop(sc, &sc->vmx_queue[q].vxq_rxqueue);

	vmxnet3_write_cmd(sc, VMXNET3_CMD_RESET);
}

static void
vmxnet3_stop(struct ifnet *ifp, int disable)
{
	struct vmxnet3_softc *sc = ifp->if_softc;

	VMXNET3_CORE_LOCK(sc);
	vmxnet3_stop_locked(sc);
	VMXNET3_CORE_UNLOCK(sc);
}

static void
vmxnet3_txinit(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;

	txr = &txq->vxtxq_cmd_ring;
	txr->vxtxr_head = 0;
	txr->vxtxr_next = 0;
	txr->vxtxr_gen = VMXNET3_INIT_GEN;
	memset(txr->vxtxr_txd, 0,
	    txr->vxtxr_ndesc * sizeof(struct vmxnet3_txdesc));

	txc = &txq->vxtxq_comp_ring;
	txc->vxcr_next = 0;
	txc->vxcr_gen = VMXNET3_INIT_GEN;
	memset(txc->vxcr_u.txcd, 0,
	    txc->vxcr_ndesc * sizeof(struct vmxnet3_txcompdesc));
}

static int
vmxnet3_rxinit(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	u_int i, populate, idx;
	int error;

	/* LRO and jumbo frame is not supported yet */
	populate = 1;

	for (i = 0; i < populate; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_fill = 0;
		rxr->vxrxr_gen = VMXNET3_INIT_GEN;
		memset(rxr->vxrxr_rxd, 0,
		    rxr->vxrxr_ndesc * sizeof(struct vmxnet3_rxdesc));

		for (idx = 0; idx < rxr->vxrxr_ndesc; idx++) {
			error = vmxnet3_newbuf(sc, rxq, rxr);
			if (error)
				return (error);
		}
	}

	for (/**/; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_fill = 0;
		rxr->vxrxr_gen = 0;
		memset(rxr->vxrxr_rxd, 0,
		    rxr->vxrxr_ndesc * sizeof(struct vmxnet3_rxdesc));
	}

	rxc = &rxq->vxrxq_comp_ring;
	rxc->vxcr_next = 0;
	rxc->vxcr_gen = VMXNET3_INIT_GEN;
	memset(rxc->vxcr_u.rxcd, 0,
	    rxc->vxcr_ndesc * sizeof(struct vmxnet3_rxcompdesc));

	return (0);
}

static int
vmxnet3_reinit_queues(struct vmxnet3_softc *sc)
{
	device_t dev;
	int q, error;
	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_ntxqueues; q++)
		vmxnet3_txinit(sc, &sc->vmx_queue[q].vxq_txqueue);

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		error = vmxnet3_rxinit(sc, &sc->vmx_queue[q].vxq_rxqueue);
		if (error) {
			device_printf(dev, "cannot populate Rx queue %d\n", q);
			return (error);
		}
	}

	return (0);
}

static int
vmxnet3_enable_device(struct vmxnet3_softc *sc)
{
	int q;

	if (vmxnet3_read_cmd(sc, VMXNET3_CMD_ENABLE) != 0) {
		device_printf(sc->vmx_dev, "device enable command failed!\n");
		return (1);
	}

	/* Reset the Rx queue heads. */
	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_RXH1(q), 0);
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_RXH2(q), 0);
	}

	return (0);
}

static void
vmxnet3_reinit_rxfilters(struct vmxnet3_softc *sc)
{

	vmxnet3_set_rxfilter(sc);

	memset(sc->vmx_ds->vlan_filter, 0, sizeof(sc->vmx_ds->vlan_filter));
	vmxnet3_write_cmd(sc, VMXNET3_CMD_VLAN_FILTER);
}

static int
vmxnet3_reinit(struct vmxnet3_softc *sc)
{

	vmxnet3_set_lladdr(sc);
	vmxnet3_reinit_shared_data(sc);

	if (vmxnet3_reinit_queues(sc) != 0)
		return (ENXIO);

	if (vmxnet3_enable_device(sc) != 0)
		return (ENXIO);

	vmxnet3_reinit_rxfilters(sc);

	return (0);
}

static int
vmxnet3_init_locked(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp = &sc->vmx_ethercom.ec_if;
	int error;

	vmxnet3_stop_locked(sc);

	error = vmxnet3_reinit(sc);
	if (error) {
		vmxnet3_stop_locked(sc);
		return (error);
	}

	ifp->if_flags |= IFF_RUNNING;
	vmxnet3_if_link_status(sc);

	vmxnet3_enable_all_intrs(sc);
	callout_reset(&sc->vmx_tick, hz, vmxnet3_tick, sc);

	return (0);
}

static int
vmxnet3_init(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc = ifp->if_softc;
	int error;

	VMXNET3_CORE_LOCK(sc);
	error = vmxnet3_init_locked(sc);
	VMXNET3_CORE_UNLOCK(sc);

	return (error);
}

static int
vmxnet3_txq_offload_ctx(struct vmxnet3_txqueue *txq, struct mbuf *m,
    int *start, int *csum_start)
{
	struct ether_header *eh;
	struct mbuf *mp;
	int offset, csum_off, iphl, offp;
	bool v4;

	eh = mtod(m, struct ether_header *);
	switch (htons(eh->ether_type)) {
	case ETHERTYPE_IP:
	case ETHERTYPE_IPV6:
		offset = ETHER_HDR_LEN;
		break;
	case ETHERTYPE_VLAN:
		offset = ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN;
		break;
	default:
		m_freem(m);
		return (EINVAL);
	}

	if ((m->m_pkthdr.csum_flags &
	    (M_CSUM_TSOv4 | M_CSUM_UDPv4 | M_CSUM_TCPv4)) != 0) {
		iphl = M_CSUM_DATA_IPv4_IPHL(m->m_pkthdr.csum_data);
		v4 = true;
	} else {
		iphl = M_CSUM_DATA_IPv6_IPHL(m->m_pkthdr.csum_data);
		v4 = false;
	}
	*start = offset + iphl;

	if (m->m_pkthdr.csum_flags &
	    (M_CSUM_TCPv4 | M_CSUM_TCPv6 | M_CSUM_TSOv4 | M_CSUM_TSOv6)) {
		csum_off = offsetof(struct tcphdr, th_sum);
	} else {
		csum_off = offsetof(struct udphdr, uh_sum);
	}

	*csum_start = *start + csum_off;
	mp = m_pulldown(m, 0, *csum_start + 2, &offp);
	if (!mp) {
		/* m is already freed */
		return ENOBUFS;
	}

	if (m->m_pkthdr.csum_flags & (M_CSUM_TSOv4 | M_CSUM_TSOv6)) {
		struct tcphdr *tcp;

		txq->vxtxq_stats.vmtxs_tso++;
		tcp = (void *)(mtod(mp, char *) + offp + *start);

		if (v4) {
			struct ip *ip;

			ip = (void *)(mtod(mp, char *) + offp + offset);
			tcp->th_sum = in_cksum_phdr(ip->ip_src.s_addr,
			    ip->ip_dst.s_addr, htons(IPPROTO_TCP));
		} else {
			struct ip6_hdr *ip6;

			ip6 = (void *)(mtod(mp, char *) + offp + offset);
			tcp->th_sum = in6_cksum_phdr(&ip6->ip6_src,
			    &ip6->ip6_dst, 0, htonl(IPPROTO_TCP));
		}

		/*
		 * For TSO, the size of the protocol header is also
		 * included in the descriptor header size.
		 */
		*start += (tcp->th_off << 2);
	} else
		txq->vxtxq_stats.vmtxs_csum++;

	return (0);
}

static int
vmxnet3_txq_load_mbuf(struct vmxnet3_txqueue *txq, struct mbuf **m0,
    bus_dmamap_t dmap)
{
	struct mbuf *m;
	bus_dma_tag_t tag;
	int error;

	m = *m0;
	tag = txq->vxtxq_sc->vmx_dmat;

	error = bus_dmamap_load_mbuf(tag, dmap, m, BUS_DMA_NOWAIT);
	if (error == 0 || error != EFBIG)
		return (error);

	m = m_defrag(m, M_NOWAIT);
	if (m != NULL) {
		*m0 = m;
		error = bus_dmamap_load_mbuf(tag, dmap, m, BUS_DMA_NOWAIT);
	} else
		error = ENOBUFS;

	if (error) {
		m_freem(*m0);
		*m0 = NULL;
		txq->vxtxq_defrag_failed.ev_count++;
	} else
		txq->vxtxq_defragged.ev_count++;

	return (error);
}

static void
vmxnet3_txq_unload_mbuf(struct vmxnet3_txqueue *txq, bus_dmamap_t dmap)
{

	bus_dmamap_unload(txq->vxtxq_sc->vmx_dmat, dmap);
}

static int
vmxnet3_txq_encap(struct vmxnet3_txqueue *txq, struct mbuf **m0)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txring *txr;
	struct vmxnet3_txdesc *txd, *sop;
	struct mbuf *m;
	bus_dmamap_t dmap;
	bus_dma_segment_t *segs;
	int i, gen, start, csum_start, nsegs, error;

	sc = txq->vxtxq_sc;
	start = 0;
	txd = NULL;
	txr = &txq->vxtxq_cmd_ring;
	dmap = txr->vxtxr_txbuf[txr->vxtxr_head].vtxb_dmamap;
	csum_start = 0; /* GCC */

	error = vmxnet3_txq_load_mbuf(txq, m0, dmap);
	if (error)
		return (error);

	nsegs = dmap->dm_nsegs;
	segs = dmap->dm_segs;

	m = *m0;
	KASSERT(m->m_flags & M_PKTHDR);
	KASSERT(nsegs <= VMXNET3_TX_MAXSEGS);

	if (vmxnet3_txring_avail(txr) < nsegs) {
		txq->vxtxq_stats.vmtxs_full++;
		vmxnet3_txq_unload_mbuf(txq, dmap);
		return (ENOSPC);
	} else if (m->m_pkthdr.csum_flags & VMXNET3_CSUM_ALL_OFFLOAD) {
		error = vmxnet3_txq_offload_ctx(txq, m, &start, &csum_start);
		if (error) {
			/* m is already freed */
			txq->vxtxq_stats.vmtxs_offload_failed++;
			vmxnet3_txq_unload_mbuf(txq, dmap);
			*m0 = NULL;
			return (error);
		}
	}

	txr->vxtxr_txbuf[txr->vxtxr_head].vtxb_m = m;
	sop = &txr->vxtxr_txd[txr->vxtxr_head];
	gen = txr->vxtxr_gen ^ 1;	/* Owned by cpu (yet) */

	for (i = 0; i < nsegs; i++) {
		txd = &txr->vxtxr_txd[txr->vxtxr_head];

		txd->addr = segs[i].ds_addr;
		txd->len = segs[i].ds_len;
		txd->gen = gen;
		txd->dtype = 0;
		txd->offload_mode = VMXNET3_OM_NONE;
		txd->offload_pos = 0;
		txd->hlen = 0;
		txd->eop = 0;
		txd->compreq = 0;
		txd->vtag_mode = 0;
		txd->vtag = 0;

		if (++txr->vxtxr_head == txr->vxtxr_ndesc) {
			txr->vxtxr_head = 0;
			txr->vxtxr_gen ^= 1;
		}
		gen = txr->vxtxr_gen;
	}
	txd->eop = 1;
	txd->compreq = 1;

	if (vlan_has_tag(m)) {
		sop->vtag_mode = 1;
		sop->vtag = vlan_get_tag(m);
	}

	if (m->m_pkthdr.csum_flags & (M_CSUM_TSOv4 | M_CSUM_TSOv6)) {
		sop->offload_mode = VMXNET3_OM_TSO;
		sop->hlen = start;
		sop->offload_pos = m->m_pkthdr.segsz;
	} else if (m->m_pkthdr.csum_flags & (VMXNET3_CSUM_OFFLOAD |
	    VMXNET3_CSUM_OFFLOAD_IPV6)) {
		sop->offload_mode = VMXNET3_OM_CSUM;
		sop->hlen = start;
		sop->offload_pos = csum_start;
	}

	/* Finally, change the ownership. */
	vmxnet3_barrier(sc, VMXNET3_BARRIER_WR);
	sop->gen ^= 1;

	txq->vxtxq_ts->npending += nsegs;
	if (txq->vxtxq_ts->npending >= txq->vxtxq_ts->intr_threshold) {
		struct vmxnet3_queue *vmxq;
		vmxq = container_of(txq, struct vmxnet3_queue, vxq_txqueue);
		txq->vxtxq_ts->npending = 0;
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_TXH(vmxq->vxq_id),
		    txr->vxtxr_head);
	}

	return (0);
}

#define VMXNET3_TX_START 1
#define VMXNET3_TX_TRANSMIT 2
static inline void
vmxnet3_tx_common_locked(struct ifnet *ifp, struct vmxnet3_txqueue *txq, int txtype)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txring *txr;
	struct mbuf *m_head;
	int tx;

	sc = ifp->if_softc;
	txr = &txq->vxtxq_cmd_ring;
	tx = 0;

	VMXNET3_TXQ_LOCK_ASSERT(txq);

	if ((ifp->if_flags & IFF_RUNNING) == 0 ||
	    sc->vmx_link_active == 0)
		return;

	for (;;) {
		if (txtype == VMXNET3_TX_START)
			IFQ_POLL(&ifp->if_snd, m_head);
		else
			m_head = pcq_peek(txq->vxtxq_interq);
		if (m_head == NULL)
			break;

		if (vmxnet3_txring_avail(txr) < VMXNET3_TX_MAXSEGS)
			break;

		if (txtype == VMXNET3_TX_START)
			IFQ_DEQUEUE(&ifp->if_snd, m_head);
		else
			m_head = pcq_get(txq->vxtxq_interq);
		if (m_head == NULL)
			break;

		if (vmxnet3_txq_encap(txq, &m_head) != 0) {
			if (m_head != NULL)
				m_freem(m_head);
			break;
		}

		tx++;
		bpf_mtap(ifp, m_head, BPF_D_OUT);
	}

	if (tx > 0)
		txq->vxtxq_watchdog = VMXNET3_WATCHDOG_TIMEOUT;
}

static void
vmxnet3_start_locked(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;

	sc = ifp->if_softc;
	txq = &sc->vmx_queue[0].vxq_txqueue;

	vmxnet3_tx_common_locked(ifp, txq, VMXNET3_TX_START);
}

void
vmxnet3_start(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;

	sc = ifp->if_softc;
	txq = &sc->vmx_queue[0].vxq_txqueue;

	VMXNET3_TXQ_LOCK(txq);
	vmxnet3_start_locked(ifp);
	VMXNET3_TXQ_UNLOCK(txq);
}

static int
vmxnet3_select_txqueue(struct ifnet *ifp, struct mbuf *m __unused)
{
	struct vmxnet3_softc *sc;
	u_int cpuid;

	sc = ifp->if_softc;
	cpuid = cpu_index(curcpu());
	/*
	 * Furure work
	 * We should select txqueue to even up the load even if ncpu is
	 * different from sc->vmx_ntxqueues. Currently, the load is not
	 * even, that is, when ncpu is six and ntxqueues is four, the load
	 * of vmx_queue[0] and vmx_queue[1] is higher than vmx_queue[2] and
	 * vmx_queue[3] because CPU#4 always uses vmx_queue[0] and CPU#5 always
	 * uses vmx_queue[1].
	 * Furthermore, we should not use random value to select txqueue to
	 * avoid reordering. We should use flow information of mbuf.
	 */
	return cpuid % sc->vmx_ntxqueues;
}

static void
vmxnet3_transmit_locked(struct ifnet *ifp, struct vmxnet3_txqueue *txq)
{

	vmxnet3_tx_common_locked(ifp, txq, VMXNET3_TX_TRANSMIT);
}

static int
vmxnet3_transmit(struct ifnet *ifp, struct mbuf *m)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;
	int qid;

	qid = vmxnet3_select_txqueue(ifp, m);
	sc = ifp->if_softc;
	txq = &sc->vmx_queue[qid].vxq_txqueue;

	if (__predict_false(!pcq_put(txq->vxtxq_interq, m))) {
		VMXNET3_TXQ_LOCK(txq);
		txq->vxtxq_pcqdrop.ev_count++;
		VMXNET3_TXQ_UNLOCK(txq);
		m_freem(m);
		return ENOBUFS;
	}

	if (VMXNET3_TXQ_TRYLOCK(txq)) {
		vmxnet3_transmit_locked(ifp, txq);
		VMXNET3_TXQ_UNLOCK(txq);
	} else {
		kpreempt_disable();
		softint_schedule(txq->vxtxq_si);
		kpreempt_enable();
	}

	return 0;
}

static void
vmxnet3_deferred_transmit(void *arg)
{
	struct vmxnet3_txqueue *txq = arg;
	struct vmxnet3_softc *sc = txq->vxtxq_sc;
	struct ifnet *ifp = &sc->vmx_ethercom.ec_if;

	VMXNET3_TXQ_LOCK(txq);
	txq->vxtxq_transmitdef.ev_count++;
	if (pcq_peek(txq->vxtxq_interq) != NULL)
		vmxnet3_transmit_locked(ifp, txq);
	VMXNET3_TXQ_UNLOCK(txq);
}

static void
vmxnet3_set_rxfilter(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp = &sc->vmx_ethercom.ec_if;
	struct ethercom *ec = &sc->vmx_ethercom;
	struct vmxnet3_driver_shared *ds = sc->vmx_ds;
	struct ether_multi *enm;
	struct ether_multistep step;
	u_int mode;
	uint8_t *p;

	ds->mcast_tablelen = 0;
	ETHER_LOCK(ec);
	CLR(ec->ec_flags, ETHER_F_ALLMULTI);
	ETHER_UNLOCK(ec);

	/*
	 * Always accept broadcast frames.
	 * Always accept frames destined to our station address.
	 */
	mode = VMXNET3_RXMODE_BCAST | VMXNET3_RXMODE_UCAST;

	ETHER_LOCK(ec);
	if (ISSET(ifp->if_flags, IFF_PROMISC) ||
	    ec->ec_multicnt > VMXNET3_MULTICAST_MAX)
		goto allmulti;

	p = sc->vmx_mcast;
	ETHER_FIRST_MULTI(step, ec, enm);
	while (enm != NULL) {
		if (memcmp(enm->enm_addrlo, enm->enm_addrhi, ETHER_ADDR_LEN)) {
			/*
			 * We must listen to a range of multicast addresses.
			 * For now, just accept all multicasts, rather than
			 * trying to set only those filter bits needed to match
			 * the range.  (At this time, the only use of address
			 * ranges is for IP multicast routing, for which the
			 * range is big enough to require all bits set.)
			 */
			goto allmulti;
		}
		memcpy(p, enm->enm_addrlo, ETHER_ADDR_LEN);

		p += ETHER_ADDR_LEN;

		ETHER_NEXT_MULTI(step, enm);
	}

	if (ec->ec_multicnt > 0) {
		SET(mode, VMXNET3_RXMODE_MCAST);
		ds->mcast_tablelen = p - sc->vmx_mcast;
	}
	ETHER_UNLOCK(ec);

	goto setit;

allmulti:
	SET(ec->ec_flags, ETHER_F_ALLMULTI);
	ETHER_UNLOCK(ec);
	SET(mode, (VMXNET3_RXMODE_ALLMULTI | VMXNET3_RXMODE_MCAST));
	if (ifp->if_flags & IFF_PROMISC)
		SET(mode, VMXNET3_RXMODE_PROMISC);

setit:
	vmxnet3_write_cmd(sc, VMXNET3_CMD_SET_FILTER);
	ds->rxmode = mode;
	vmxnet3_write_cmd(sc, VMXNET3_CMD_SET_RXMODE);
}

static int
vmxnet3_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct vmxnet3_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	int s, error = 0;

	switch (cmd) {
	case SIOCSIFMTU: {
		int nmtu = ifr->ifr_mtu;

		if (nmtu < VMXNET3_MIN_MTU || nmtu > VMXNET3_MAX_MTU) {
			error = EINVAL;
			break;
		}
		if (ifp->if_mtu != (uint64_t)nmtu) {
			s = splnet();
			error = ether_ioctl(ifp, cmd, data);
			splx(s);
			if (error == ENETRESET)
				error = vmxnet3_init(ifp);
		}
		break;
	}

	default:
		s = splnet();
		error = ether_ioctl(ifp, cmd, data);
		splx(s);
	}

	if (error == ENETRESET) {
		VMXNET3_CORE_LOCK(sc);
		if (ifp->if_flags & IFF_RUNNING)
			vmxnet3_set_rxfilter(sc);
		VMXNET3_CORE_UNLOCK(sc);
		error = 0;
	}

	return error;
}

static int
vmxnet3_ifflags_cb(struct ethercom *ec)
{
	struct vmxnet3_softc *sc;

	sc = ec->ec_if.if_softc;

	VMXNET3_CORE_LOCK(sc);
	vmxnet3_set_rxfilter(sc);
	VMXNET3_CORE_UNLOCK(sc);

	vmxnet3_if_link_status(sc);

	return 0;
}

static int
vmxnet3_watchdog(struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_queue *vmxq;

	sc = txq->vxtxq_sc;
	vmxq = container_of(txq, struct vmxnet3_queue, vxq_txqueue);

	VMXNET3_TXQ_LOCK(txq);
	if (txq->vxtxq_watchdog == 0 || --txq->vxtxq_watchdog) {
		VMXNET3_TXQ_UNLOCK(txq);
		return (0);
	}
	txq->vxtxq_watchdogto.ev_count++;
	VMXNET3_TXQ_UNLOCK(txq);

	device_printf(sc->vmx_dev, "watchdog timeout on queue %d\n",
	    vmxq->vxq_id);
	return (1);
}

static void
vmxnet3_refresh_host_stats(struct vmxnet3_softc *sc)
{

	vmxnet3_write_cmd(sc, VMXNET3_CMD_GET_STATS);
}

static void
vmxnet3_tick(void *xsc)
{
	struct vmxnet3_softc *sc;
	int i, timedout;

	sc = xsc;
	timedout = 0;

	VMXNET3_CORE_LOCK(sc);

	vmxnet3_refresh_host_stats(sc);

	for (i = 0; i < sc->vmx_ntxqueues; i++)
		timedout |= vmxnet3_watchdog(&sc->vmx_queue[i].vxq_txqueue);

	if (timedout != 0)
		vmxnet3_init_locked(sc);
	else
		callout_reset(&sc->vmx_tick, hz, vmxnet3_tick, sc);

	VMXNET3_CORE_UNLOCK(sc);
}

/*
 * update link state of ifnet and softc
 */
static void
vmxnet3_if_link_status(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp = &sc->vmx_ethercom.ec_if;
	u_int link;
	bool up;

	up = vmxnet3_cmd_link_status(ifp);
	if (up) {
		sc->vmx_link_active = 1;
		link = LINK_STATE_UP;
	} else {
		sc->vmx_link_active = 0;
		link = LINK_STATE_DOWN;
	}

	if_link_state_change(ifp, link);
}

/*
 * check vmx(4) state by VMXNET3_CMD and update ifp->if_baudrate
 *   returns
 *       - true:  link up
 *       - flase: link down
 */
static bool
vmxnet3_cmd_link_status(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc = ifp->if_softc;
	u_int x, speed;

	x = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_LINK);
	if ((x & 1) == 0)
		return false;

	speed = x >> 16;
	ifp->if_baudrate = IF_Mbps(speed);
	return true;
}

static void
vmxnet3_ifmedia_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	bool up;

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;

	up = vmxnet3_cmd_link_status(ifp);
	if (!up)
		return;

	ifmr->ifm_status |= IFM_ACTIVE;

	if (ifp->if_baudrate >= IF_Gbps(10ULL))
		ifmr->ifm_active |= IFM_10G_T;
}

static int
vmxnet3_ifmedia_change(struct ifnet *ifp)
{
	return 0;
}

static void
vmxnet3_set_lladdr(struct vmxnet3_softc *sc)
{
	uint32_t ml, mh;

	ml  = sc->vmx_lladdr[0];
	ml |= sc->vmx_lladdr[1] << 8;
	ml |= sc->vmx_lladdr[2] << 16;
	ml |= sc->vmx_lladdr[3] << 24;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_MACL, ml);

	mh  = sc->vmx_lladdr[4];
	mh |= sc->vmx_lladdr[5] << 8;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_MACH, mh);
}

static void
vmxnet3_get_lladdr(struct vmxnet3_softc *sc)
{
	uint32_t ml, mh;

	ml = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_MACL);
	mh = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_MACH);

	sc->vmx_lladdr[0] = ml;
	sc->vmx_lladdr[1] = ml >> 8;
	sc->vmx_lladdr[2] = ml >> 16;
	sc->vmx_lladdr[3] = ml >> 24;
	sc->vmx_lladdr[4] = mh;
	sc->vmx_lladdr[5] = mh >> 8;
}

static void
vmxnet3_enable_all_intrs(struct vmxnet3_softc *sc)
{
	int i;

	sc->vmx_ds->ictrl &= ~VMXNET3_ICTRL_DISABLE_ALL;
	for (i = 0; i < sc->vmx_nintrs; i++)
		vmxnet3_enable_intr(sc, i);
}

static void
vmxnet3_disable_all_intrs(struct vmxnet3_softc *sc)
{
	int i;

	sc->vmx_ds->ictrl |= VMXNET3_ICTRL_DISABLE_ALL;
	for (i = 0; i < sc->vmx_nintrs; i++)
		vmxnet3_disable_intr(sc, i);
}

static int
vmxnet3_dma_malloc(struct vmxnet3_softc *sc, bus_size_t size, bus_size_t align,
    struct vmxnet3_dma_alloc *dma)
{
	bus_dma_tag_t t = sc->vmx_dmat;
	bus_dma_segment_t *segs = dma->dma_segs;
	int n, error;

	memset(dma, 0, sizeof(*dma));

	error = bus_dmamem_alloc(t, size, align, 0, segs, 1, &n, BUS_DMA_NOWAIT);
	if (error) {
		aprint_error_dev(sc->vmx_dev, "bus_dmamem_alloc failed: %d\n", error);
		goto fail1;
	}
	KASSERT(n == 1);

	error = bus_dmamem_map(t, segs, 1, size, &dma->dma_vaddr, BUS_DMA_NOWAIT);
	if (error) {
		aprint_error_dev(sc->vmx_dev, "bus_dmamem_map failed: %d\n", error);
		goto fail2;
	}

	error = bus_dmamap_create(t, size, 1, size, 0, BUS_DMA_NOWAIT, &dma->dma_map);
	if (error) {
		aprint_error_dev(sc->vmx_dev, "bus_dmamap_create failed: %d\n", error);
		goto fail3;
	}

	error = bus_dmamap_load(t, dma->dma_map, dma->dma_vaddr, size, NULL,
	    BUS_DMA_NOWAIT);
	if (error) {
		aprint_error_dev(sc->vmx_dev, "bus_dmamap_load failed: %d\n", error);
		goto fail4;
	}

	memset(dma->dma_vaddr, 0, size);
	dma->dma_paddr = DMAADDR(dma->dma_map);
	dma->dma_size = size;

	return (0);
fail4:
	bus_dmamap_destroy(t, dma->dma_map);
fail3:
	bus_dmamem_unmap(t, dma->dma_vaddr, size);
fail2:
	bus_dmamem_free(t, segs, 1);
fail1:
	return (error);
}

static void
vmxnet3_dma_free(struct vmxnet3_softc *sc, struct vmxnet3_dma_alloc *dma)
{
	bus_dma_tag_t t = sc->vmx_dmat;

	bus_dmamap_unload(t, dma->dma_map);
	bus_dmamap_destroy(t, dma->dma_map);
	bus_dmamem_unmap(t, dma->dma_vaddr, dma->dma_size);
	bus_dmamem_free(t, dma->dma_segs, 1);

	memset(dma, 0, sizeof(*dma));
}

MODULE(MODULE_CLASS_DRIVER, if_vmx, "pci");

#ifdef _MODULE
#include "ioconf.c"
#endif

static int
if_vmx_modcmd(modcmd_t cmd, void *opaque)
{
	int error = 0;

	switch (cmd) {
	case MODULE_CMD_INIT:
#ifdef _MODULE
		error = config_init_component(cfdriver_ioconf_if_vmx,
		    cfattach_ioconf_if_vmx, cfdata_ioconf_if_vmx);
#endif
		return error;
	case MODULE_CMD_FINI:
#ifdef _MODULE
		error = config_fini_component(cfdriver_ioconf_if_vmx,
		    cfattach_ioconf_if_vmx, cfdata_ioconf_if_vmx);
#endif
		return error;
	default:
		return ENOTTY;
	}
}

