#include "ib.h"

struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest);
struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx); 
int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx);

struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints;
	memset (&hints, 0, sizeof(struct addrinfo));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char *service;
	//char *msg = (char *) malloc(sizeof(pingpong_dest));
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

	//gid_to_wire_gid(&my_dest->gid, gid);
	//sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
	//						my_dest->psn, gid);

	/*
		send information to server
	*/
	if (write(sockfd, (void *)my_dest, sizeof(*my_dest)) != sizeof(*my_dest)) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	/*
		read information to rem_dest
	*/
	if (read(sockfd, (void *) rem_dest, sizeof *rem_dest) != sizeof *rem_dest) {
		perror("client read/write");
		fprintf(stderr, "Couldn't read/write remote address\n");
		goto out;
	}


	//sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
	//					&rem_dest->psn, gid);
	//wire_gid_to_gid(gid, &rem_dest->gid);


out:
	close(sockfd);
	return rem_dest;
}


struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
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
	//char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	char msg;
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

	/*
		read information from client
	*/
	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	n = read(connfd, (void *)rem_dest, sizeof *rem_dest);
	if (n != sizeof *rem_dest) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	//sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
	//						&rem_dest->psn, gid);

	//wire_gid_to_gid(gid, &rem_dest->gid);

	if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
								sgid_idx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	//gid_to_wire_gid(&my_dest->gid, gid);
	//sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
	//						my_dest->psn, gid);
	
	if (write(connfd, (void *)my_dest, sizeof *my_dest) != sizeof *my_dest) {
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
connect QPs from two ends
*/
int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
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