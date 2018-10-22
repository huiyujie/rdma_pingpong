/*
simple function the same as ibv_rc_pingpong
*/
#define _GNU_SOURCE
#include "ib.h"

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);


static int page_size;


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

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE | 
			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
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
			.max_send_wr  = 10,
			.max_recv_wr  = 10,
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
		.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
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
parse single work request

return 0: success
return 1: failed
*/
static inline int parse_single_wc(struct pingpong_context *ctx,
				  int *rcnt, int iters,
				  uint64_t wr_id, enum ibv_wc_status status)
{
	if (status != IBV_WC_SUCCESS) {
		fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
			ibv_wc_status_str(status),
			status, (int)wr_id);
		return 1;
	}

	switch ((int) wr_id) {
		case PINGPONG_READ_WRID:
			printf("parse one read\n");
			++(*rcnt);
			break;
		default:
			fprintf(stderr, "Completion for unknown wr_id %d\n",
			(int)wr_id);
			return 1;	
	}
		
	//ctx->pending &= ~(int)wr_id;
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

	if (argc == 2) {
		servername = strdupa(argv[1]);
		printf("servername : %s", servername);
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

	//routs = pp_post_recv(ctx, ctx->rx_depth);

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
	my_dest.key = (uint32_t)ctx->mr->rkey;
	my_dest.addr = (uint64_t)ctx->mr->addr;

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

	printf("success connected\n");

	if (servername) {
		if (pp_post_write(ctx, rem_dest)){
			fprintf(stderr, "couldn't post write\n");
			return 1;
		}
	} else {
		if(pp_post_recv(ctx)){
			fprintf(stderr, "failed to post recv of the finised message\n");
			return 1;
		}
	}

	if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		return 1;
	}
	printf("start count\n");
	if (servername) {
		int ret;
		rcnt = 0;
		while (rcnt < iters) {
			ret = pp_post_read(ctx, rem_dest);
			if (ret){
				fprintf(stderr, "failed post read\n");
				printf("error number %d\n", ret);
				return 1;
			}
			int ret;

			int ne, i;
			struct ibv_wc wc;
			do {
				ne = ibv_poll_cq(ctx->cq, 1, &wc);
				if (ne < 0) {
					fprintf(stderr, "poll CQ failed %d\n", ne);
					return 1;
				}
			} while(ne < 1);
			printf("get one work completion\n");

			ret = parse_single_wc(ctx, &rcnt, iters, wc.wr_id, wc.status);
			if (ret) {
				fprintf(stderr, "parse WC failed %d\n", ne);
				return 1;
			}

			if (pp_post_write(ctx, rem_dest)){
				fprintf(stderr, "failed post write\n");
				return 1;
			}
		}
		pp_post_send(ctx);
		printf("post finished message\n");
	} else {
		struct ibv_wc wc;
		int ne;

		do {
			ne = ibv_poll_cq(ctx->cq, 1, &wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		} while (ne < 1);
		printf("recieve finished message\n");
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









