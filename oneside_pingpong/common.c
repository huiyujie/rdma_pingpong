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

static const int RDMA_BUFFER_SIZE = 4096;

/*
	Definition of 'rdma_cm_id' in 'rdma_cma.h'
-----------------------------------------------------
	struct rdma_cm_id {
		struct ibv_context	*verbs;
		struct rdma_event_channel *channel;
		void			*context;
		struct ibv_qp		*qp;
		struct rdma_route	 route;
		enum rdma_port_space	 ps;
		uint8_t			 port_num;
		struct rdma_cm_event	*event;
		struct ibv_comp_channel *send_cq_channel;
		struct ibv_cq		*send_cq;
		struct ibv_comp_channel *recv_cq_channel;
		struct ibv_cq		*recv_cq;
		struct ibv_srq		*srq;
		struct ibv_pd		*pd;
		enum ibv_qp_type	qp_type;
	};
*/





struct pingpong_context {
	struct ibv_context *ctx;
	struct ibv_comp_channel *channel;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
};

struct connection {
	struct rdma_cm_id *id;
	struct ibv_qp *qp;

	struct ibv_mr *recv_mr;
	struct ibv_mr *send_mr;
	struct ibv_mr *rdma_local_mr;
	struct ibv_mr *rdma_remote_mr;
};

static struct pingpong_context *pp_ctx = NULL;




/*
	build connection and connect this context pointer to rdma_cm_id
*/
void build_connection(struct rdma_cm_id *id){
	struct connection *conn;
	struct ibv_qp_init_attr qp_attr;


	build_context(id->verbs);//verbs is a ibv_context filed of rdma_cm_id
	build_qp_attr(&qp_attr);

	if (rdma_create_qp(id, pp_ctx->pd, &qp_attr) != 0){
		printf("falied to create queue pair\n");
		return 0;
	}

	/*
		this context is user specified context associated with the rdma_cm_id
	*/
	id->context = conn = (struct connection *)malloc(sizeof(struct connection));
	conn->id = id;
	conn->qp = id->qp;

	register_memory();
}

/*
	build context structure, create protection domain, create completion queue and channel.
*/
void build_context(struct ibv_context *verbs) {
	pp_ctx = (struct pingpong_context *) malloc(sizeof(pingpong_context));
	pp_ctx->ctx = verbs;
	pp_ctx->pd = ibv_alloc_pd(verbs);
	if (!pp_ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}
	pp_ctx->channel = ibv_create_comp_channel(verbs);
	if (!pp_ctx->channel) {
		printf("Couldn't create completion channel\n");
		goto clean_comp_channel;
	}
	pp_ctx->cq = ibv_create_cq(verbs, 10, NULL, pp_ctx->channel, 0);
	if (!pp_ctx->cq){
		printf("Couldn't create completion queue\n");
		goto clean_comp_channel;
	}

}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr){
	memset(qp_attr, 0, sizeof(*qp_attr));

  	qp_attr->send_cq = pp_ctx->cq;
	qp_attr->recv_cq = pp_ctx->cq;
	qp_attr->qp_type = IBV_QPT_RC;

	qp_attr->cap.max_send_wr = 1;
	qp_attr->cap.max_recv_wr = 1;
	qp_attr->cap.max_send_sge = 1;
	qp_attr->cap.max_recv_sge = 1;
}

/*
register memory for read/write or send/recv
*/
void register_memory(struct connection *conn){
	conn->rdma_local_mr = malloc(RDMA_BUFFER_SIZE);
	conn->rdma_remote_mr = malloc(RDMA_BUFFER_SIZE);

	
}



