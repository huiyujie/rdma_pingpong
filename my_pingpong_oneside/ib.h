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

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
	PINGPONG_READ_WRID = 3,
	PINGPONG_WRITE_WRID = 4,
};

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

	uint32_t key;
	uint64_t addr;
};


int pp_post_read(struct pingpong_context *ctx, struct pingpong_dest *rem_dest);
int pp_post_write(struct pingpong_context *ctx, struct pingpong_dest *rem_dest);
int pp_post_recv(struct pingpong_context *ctx);
int pp_post_send(struct pingpong_context *ctx);

