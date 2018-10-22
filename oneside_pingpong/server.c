


int main(int argc, char **argv) {
	struct addrinfo addr;
	struct rdma_cm_event *event = NULL;
	struct rdma_cm_id *listener = NULL;
	struct rdma_event_channel *ec = NULL;
	uint16_t port = 0;
	int ret = 0;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;


	/*
		create event channel which is used for processing connect event.
	*/
	ec = rdma_create_event_channel();
	if (ec == 0){
		printf("failed to create event channel\n");
		return 0;
	}

	/*
	  allocate a communication identifier which is similiar as socket in tcp/ip
	*/
	if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP) != 0) {
		printf("failed to create rdma id.\n");
		return 0;
	}

	/*
	*/
	if (rdma_bind_addr(listener, (struct sockaddr *)&addr) != 0) {
		printf("failed to bind address to id\n");
		rdma_destroy_id(listener);
		return 0;
	}

	/*
	*/
	if (rdma_listen(listener, 5) != 0) {
		printf("failed to listen\n");
		rdma_destroy_id(listener);
		return 0;
	}

	/*
		return the local port
	*/
	port = ntohs(rdma_get_src_port(listener));

	printf("listening on port %d.\n", port);

	/*
		get the connecting request event
		block
	*/
	while (rdma_get_cm_event(ec, &event) == 0) {
		if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) continue;
		else break;
	}

	





}