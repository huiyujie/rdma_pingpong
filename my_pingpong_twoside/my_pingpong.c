/*
simple function the same as ibv_rc_pingpong
*/
#define _GNU_SOURCE
#include <linux/types.h>
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <inttypes.h>
#include <infiniband/verbs.h>


void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

static int page_size;

struct pingpong_context {
	struct ibv_context *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	void *buf;
	int size;
	int rx_depth;
	int pending;
	struct ibv_port_attr portinfo;
};

struct pingpong_dest{
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};


/*
initiate the pingpong_context, allocate all the resources and create everything that 
needed for the connection
*/
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size, int rx_depth, int port) 
{
	struct pingpong_context *ctx;

	ctx=calloc(1, sizeof *ctx);
	if (!ctx) return NULL;

	ctx->size = size;
	ctx->rx_depth = rx_depth;

	ctx->buf = memalign(page_size, size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buffer\n");
		goto clean_ctx;
	}

	memset(ctx->buf, 0x7b, size);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
		goto clean_buffer;
	}

	ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_mr;
	}

	ctx->cq = ibv_create_cq(ctx->context, rx_depth+1, NULL, ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;
	}

	struct ibv_qp_init_attr init_attr = {
		.send_cq = ctx->cq,
		.recv_cq = ctx->cq,
		.cap     = {
			.max_send_wr  = 1,
			.max_recv_wr  = rx_depth,
			.max_send_sge = 1,
			.max_recv_sge = 1
		},
		.qp_type = IBV_QPT_RC
	};

	ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
	if (!ctx->qp) {
		fprintf(stderr, "Couldn't create QP\n");
		goto clean_cq;
	}

	struct ibv_qp_attr attr = {
		.qp_state        = IBV_QPS_INIT,
		.pkey_index      = 0,
		.port_num        = port,
		.qp_access_flags = 0
	};

	if (ibv_modify_qp(ctx->qp, &attr, 
				IBV_QP_STATE              |
				IBV_QP_PKEY_INDEX         |
				IBV_QP_PORT               |
				IBV_QP_ACCESS_FLAGS)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		goto clean_qp;
	}

	return ctx;

clean_qp:
	ibv_destroy_qp(ctx->qp);

clean_cq:
	ibv_destroy_cq(ctx->cq);

clean_mr:
	ibv_dereg_mr(ctx->mr);


clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_buffer:
	free(ctx->buf);

clean_ctx:
	free(ctx);

	return NULL;
}


/*
connect QPs from two ends
*/
static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx) {

	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR,
		.path_mtu		= mtu,
		.dest_qp_num	= dest->qpn,
		.rq_psn			= dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= dest->lid,
			.sl		= sl,
			.src_path_bits	= 0,
			.port_num	= port
		}
	};

	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}


static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints;
	memset (&hints, 0, sizeof(struct addrinfo));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg ||
	    write(sockfd, "done", sizeof "done") != sizeof "done") {
		perror("client read/write");
		fprintf(stderr, "Couldn't read/write remote address\n");
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
						&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	close(sockfd);
	return rem_dest;
}


static struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx) 
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1, connfd;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return NULL;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, NULL);
	close(sockfd);
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}

	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);

	wire_gid_to_gid(gid, &rem_dest->gid);

	if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
								sgid_idx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(connfd, msg, sizeof msg) != sizeof msg ||
	    read(connfd, msg, sizeof msg) != sizeof "done") {
		fprintf(stderr, "Couldn't send/recv local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

out:
	close(connfd);
	return rem_dest;
}

/*
post n receive work requests to the queue pair

return value i : work request that actually be posted
*/
static int pp_post_recv(struct pingpong_context *ctx, int n)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = (uint64_t)PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;
	int ret;
	for (i = 0; i < n; ++i){
		ret = ibv_post_recv(ctx->qp, &wr, &bad_wr);
		if (ret){
			printf("%d",ret);
			break;
		}
	}
	printf("post %d recv requests\n", n);
	return i;
}


/*
post one send request to queue pair
*/
static int pp_post_send(struct pingpong_context *ctx)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t)ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = (uint64_t) PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND,
		.send_flags = IBV_SEND_SIGNALED,
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}



/*
parse single work request

return 0: success
return 1: failed
*/
static inline int parse_single_wc(struct pingpong_context *ctx, int *scnt,
				  int *rcnt, int *routs, int iters,
				  uint64_t wr_id, enum ibv_wc_status status)
{
	if (status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
			ibv_wc_status_str(status),
			status, (int)wr_id);
		return 1;
	}

	switch ((int) wr_id) {
		case PINGPONG_SEND_WRID:
			++(*scnt);
			break;

		case PINGPONG_RECV_WRID:
			if (--(*routs) <= 1) {
			*routs += pp_post_recv(ctx, ctx->rx_depth - *routs);
			if (*routs < ctx->rx_depth) {
				fprintf(stderr,
					"Couldn't post receive (%d)\n",
					*routs);
				return 1;
				}
			}
			++(*rcnt);
			break;
		default:
			fprintf(stderr, "Completion for unknown wr_id %d\n",
			(int)wr_id);
			return 1;	
	}
		
	ctx->pending &= ~(int)wr_id;

	if (*scnt < iters && !ctx->pending) {
		if (pp_post_send(ctx)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
		ctx->pending = PINGPONG_RECV_WRID |
		PINGPONG_SEND_WRID;
	}

	return 0;

}

static int pp_close_ctx(struct pingpong_context *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	__be32 v32;
	int i;
	uint32_t tmp_gid[4];

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		tmp_gid[i] = be32toh(v32);
	}
	memcpy(gid, tmp_gid, sizeof(*gid));
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	uint32_t tmp_gid[4];
	int i;

	memcpy(tmp_gid, gid, sizeof(tmp_gid));
	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}


int main(int argc, char *argv[]){
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest my_dest;
	struct pingpong_dest *rem_dest;
	struct timeval start, end;
	char *servername = NULL;
	unsigned int port = 18515;
	int ib_port = 1;
	unsigned int size = 4096;
	enum ibv_mtu mtu = IBV_MTU_1024;
	unsigned int rx_depth = 500;
	unsigned int iters = 1000;
	int routs;
	int rcnt, scnt;
	int gidx = -1;
	char gid[33];
	int sl = 0;

	page_size = sysconf(_SC_PAGESIZE);

	int ch;
	struct option long_options[] = {
		{"size", 1, NULL, 's'},
		{"iters", 1, NULL, 'i'}
	};

	while((ch = getopt_long(argc, argv, "s:i", long_options, NULL)) != -1) {
		switch (ch) {
			case 's':
				size = strtoul(optarg, NULL, 0);
				break;
			case 'i':
				iters = strtoul(optarg, NULL, 0);
				break;
		} 
	}

	if (optind == argc - 1){
		servername = strdupa(argv[optind]);
	}

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("fail to get ib devices\n");
		return 1;
	}

	ib_dev = *dev_list;
	if (!ib_dev) {
		fprintf(stderr, "No ib device found\n");
		return 1;
	}

	ctx = pp_init_ctx(ib_dev, size, rx_depth, ib_port);
	if (!ctx) {
		return 1;
	}

	routs = pp_post_recv(ctx, ctx->rx_depth);

	/* get the  
	*/
	if (ibv_query_port(ctx->context, ib_port, &ctx->portinfo)) {
		fprintf(stderr, "failed to get port information\n");
		return 1;
	}

	my_dest.lid = ctx->portinfo.lid;
	if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
							!my_dest.lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return 1;
	}

	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
			fprintf(stderr, "can't read sgid of index %d\n", gidx);
			return 1;
		}
	} else
		memset(&my_dest.gid, 0, sizeof my_dest.gid);

	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;

	inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       my_dest.lid, my_dest.qpn, my_dest.psn, gid);

	if (servername) 
		rem_dest = pp_client_exch_dest(servername, port, &my_dest);
	else 
		rem_dest = pp_server_exch_dest(ctx, ib_port, mtu, port, sl,
								&my_dest, gidx);

	if (!rem_dest)
		return 1;

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	if (servername)
		if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest,
					gidx))
			return 1;	

	ctx->pending = PINGPONG_RECV_WRID;

	if (servername) {
		if (pp_post_send(ctx)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
		ctx->pending |= PINGPONG_SEND_WRID;
	}

	if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		return 1;
	}

	rcnt = scnt = 0;
	while (rcnt < iters || scnt < iters) {
		int ret;

		int ne, i;
		struct ibv_wc wc[2];

		do {
			ne = ibv_poll_cq(ctx->cq, 2, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		} while(ne < 1);

		for (i = 0; i < ne; ++i) {
			ret = parse_single_wc(ctx, &scnt, &rcnt, &routs, iters, wc[i].wr_id, wc[i].status);
			if (ret) {
				fprintf(stderr, "parse WC failed %d\n", ne);
				return 1;
			}
		}
	}

	if (gettimeofday(&end, NULL)) {
		perror("gettimeofday");
		return 1;
	}


	float usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
	long long bytes = (long long) size * iters * 2;

	printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
		    bytes, usec / 1000000., bytes * 8. / usec);
	printf("%d iters in %.2f seconds = %.2f usec/iter\n",
		   iters, usec / 1000000., usec / iters);

	ibv_ack_cq_events(ctx->cq, 0);

	if (pp_close_ctx(ctx))
		return 1;

	ibv_free_device_list(dev_list);
	free(rem_dest);

	return 0;
}









