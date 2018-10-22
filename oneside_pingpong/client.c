#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

static void usage();

int main(int argc, char **argv){
	struct addrinfo *addr;
	struct rdma_cm_event *event = NULL;
	struct rdma_cm_id *listener = NULL;
	struct rdma_event_channel *ec = NULL;


	if (argc != 3) {
		usage();
	}

	/*
		get server's address information
	*/
	if (getaddrinfo(argv[1], argv[2], NULL, &addr != 0)) {
		printf("failed to get address information.\n")
		return 0;
	}

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
	if (rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP) != 0) {
		printf("failed to create rdma id.\n");
		return 0;
	}

	/*
		resolve destination address and bound id to local device
	*/
	if (rdma_resolve_addr(conn, NULL, addr->ai_addr, 500) != 0){
		printf("failed to resolve address\n");
		return 0;
	}

	freeaddrinfo(addr);

	if (rdma_get_cm_event(ec, &event) != 0) {
		printf("failed to get event\n");
		return 0;
	}

	if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED){
		printf("address resolved\n");
		rdma_ack_cm_event(event);
	}

	build_connection(event->id);





}


void usage()
{
  fprintf(stderr, "usage: <server-address> <server-port>\n");
  exit(1);
}