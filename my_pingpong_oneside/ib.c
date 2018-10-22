#include "ib.h"


/*
post rdma read from remote memory
*/
int pp_post_read(struct pingpong_context *ctx, struct pingpong_dest *rem_dest)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_READ_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode 	= IBV_WR_RDMA_READ,
		.send_flags = IBV_SEND_SIGNALED,
		.wr.rdma.remote_addr = rem_dest->addr,
		.wr.rdma.rkey = rem_dest->key,
	};
	struct ibv_send_wr *bad_wr;
	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

/*
post rdma write to remote memory
*/
int pp_post_write(struct pingpong_context *ctx, struct pingpong_dest *rem_dest)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t)ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_WRITE_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_RDMA_WRITE,
		.wr.rdma.remote_addr = rem_dest->addr,
		.wr.rdma.rkey = rem_dest->key,
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}


/*
post rdma recv
*/
int pp_post_recv(struct pingpong_context *ctx)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;

	return ibv_post_recv(ctx->qp, &wr, &bad_wr);
}

/*
post rdma send
*/
int pp_post_send(struct pingpong_context *ctx)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND,
		.send_flags = IBV_SEND_SIGNALED,
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

