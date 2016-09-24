#include <sys/epoll.h>
#include <sys/socket.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <map>
#include <errno.h>
#include <time.h>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/prctl.h>
#include <signal.h>
#include <stdlib.h>
#include <netdb.h>
#include <functional>
#include <cstring>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <cstring>
#include "hck_engine.h"


const char http_request[] = "HEAD / HTTP/1.0\r\nConnection:Keep-Alive\r\n\r\n";
#define http_request_size (sizeof(http_request) - 1)
int http_resp_startlen = sizeof("HTTP/1.0 ");//or "HTTP 1.1" same length

#define READSIZE 1024
#define MAXEVENTS 16
#define TIMEOUT_RECOVER 3
#define TIMEOUT_NEW 4
#define TIMEOUT_POST 60

#define LOG_LEVEL_EMPTY         0       /* printing nothing (if not LOG_LEVEL_INFORMATION set) */
#define LOG_LEVEL_CRIT          1
#define LOG_LEVEL_ERR           2
#define LOG_LEVEL_WARNING       3
#define LOG_LEVEL_DEBUG         4
#define LOG_LEVEL_TRACE         5
#define LOG_LEVEL_INFORMATION   127     /* printing in any case no matter what level set */


void hck_log(int level, const char *fmt, ...);

extern const char *socket_path;
volatile int running = 1;

using namespace std;


/*
 * Copy src to string dst of size siz.  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
size_t
hck_strlcpy(char *dst, const char *src, size_t siz)
{
        char *d = dst;
        const char *s = src;
        size_t n = siz;

        /* Copy as many bytes as will fit */
        if (n != 0 && --n != 0) {
                do {
                        if ((*d++ = *s++) == 0)
                                break;
                } while (--n != 0);
        }

        /* Not enough room in dst, add NUL and traverse rest of src */
        if (n == 0) {
                if (siz != 0)
                        *d = '\0';                /* NUL-terminate dst */
                while (*s++)
                        ;
        }

        return(s - src - 1);        /* count does not include NUL */
}

struct cmp_map {
	bool operator()(
		const struct sockaddr_storage& lhs,
		const struct sockaddr_storage& rhs) const
	{
		return std::memcmp(&lhs, &rhs, sizeof(struct sockaddr_storage)) < 0;
	}
};

// a check
struct hck_details {
	time_t expires;
	int client_socket;
	int remote_socket;
	struct sockaddr_storage remote_connection;
	unsigned int remote_connection_len : 8;
	unsigned short position : 16;
	enum {
		connecting = 1,
		writing = 2,
		reading1 = 3,
		reading2 = 4,
		keepalive = 5,
		recovery = 6
	} state: 6;
	bool first : 1;
	bool tfo : 1;
};

// the hck system (could be exported outside of zabbix in future)
class hck_handle {
public:
	int epfd;
	map<int, struct hck_details*> sockets;
	map<struct sockaddr_storage, int, struct cmp_map> keepalived;
};

//send result from worker -> process
bool send_result(hck_handle* hck, int sock, unsigned short result){
	//Communication socket failed!
	if (sock == -1) {
		return true;
	}

	// Actually send result
	int rc = send(sock, &result, sizeof(result), 0);
	return rc >= 0;
}

static hck_details* keepalive_lookup(hck_handle* hck, unsigned int sockaddr_len, struct sockaddr_storage sockaddr, time_t now, int source) {
	map<struct sockaddr_storage, int>::iterator it;
	struct epoll_event e;
	int rc;

	it = hck->keepalived.find(sockaddr);
	if (it != hck->keepalived.end()) {
		struct hck_details* h = hck->sockets[it->second];

		assert(h->remote_connection_len == sockaddr_len);
		assert(memcmp(&h->remote_connection, &sockaddr, sockaddr_len) == 0);
		assert(h->state == hck_details::keepalive);

		//Remove from keepalive
		int erased = hck->keepalived.erase(it->first);
		assert(erased == 1);

		h->state = hck_details::recovery;
		h->position = 0;
		h->expires = now + TIMEOUT_RECOVER;
		h->client_socket = source;
		h->first = false;
		h->tfo = true;

		e.events = EPOLLOUT;
		e.data.fd = h->remote_socket;
		rc = epoll_ctl(hck->epfd, EPOLL_CTL_MOD, h->remote_socket, &e);
		if (rc < 0)
		{
			hck_log(LOG_LEVEL_WARNING, "Unable to mod socket epoll for recovery: %s", strerror(errno));
			return NULL;
		}

		return h;
	}

	return NULL;
}

static int create_new_socket(unsigned int sockaddr_len, struct sockaddr_storage sockaddr, bool fastopen = true) {
	int socket_desc;
	int rc;
	struct sockaddr* sa = (struct sockaddr*)&sockaddr;
	
	//Create socket
	socket_desc = socket(sa->sa_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (socket_desc == -1)
	{
		return -1;
	}

	//Connect to remote server
#ifdef MSG_FASTOPEN
	if (fastopen)
	{
		rc = sendto(socket_desc, http_request, http_request_size, MSG_FASTOPEN, sa, sockaddr_len);
	}
	else
	{
		rc = connect(socket_desc, sa, sockaddr_len);
	}
#else
	rc = connect(socket_desc, &sockaddr, sockaddr_len);
#endif
	if (rc != -1){
		return socket_desc;
	}
	else
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS) {
			return socket_desc;
		}
		return rc;
	}
}

static struct hck_details* create_new_hck(hck_handle* hck, unsigned int sockaddr_len, struct sockaddr_storage sockaddr, time_t now, int source, bool fastopen = true) {
	int rc, socket_desc;
	struct epoll_event e;
	struct hck_details* h = NULL;
	
	socket_desc = create_new_socket(sockaddr_len, sockaddr, fastopen);
	if (socket_desc == -1)
	{
		hck_log(LOG_LEVEL_WARNING, "Unable to create new socket: %s", strerror(errno));
		goto error;
	}

	h = new struct hck_details;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS) 
	{
#ifdef MSG_FASTOPEN
		e.events = EPOLLOUT;
		h->state = hck_details::writing;
		h->position = 0;
#else
		e.events = EPOLLIN | EPOLLOUT;
		h->state = hck_details::connecting;
#endif
	}else{
#ifdef MSG_FASTOPEN
		if (rc < http_request_size) 
		{
			e.events = EPOLLOUT;
			h->state = hck_details::writing;
			h->position = rc;
		}
		else 
		{
			e.events = EPOLLIN;
			h->state = hck_details::reading1;
			h->position = 0;
		}
#else
		e.events = EPOLLOUT;
		h->state = hck_details::writing;
		h->position = 0;
#endif
	}
	e.data.fd = socket_desc;
	rc = epoll_ctl(hck->epfd, EPOLL_CTL_ADD, socket_desc, &e);
	if (rc < 0)
	{
		hck_log(LOG_LEVEL_WARNING, "Unable to add socket to epoll: %s", strerror(errno));
		goto error;
	}

	h->expires = now + TIMEOUT_NEW;
	h->client_socket = source;
	h->remote_connection = sockaddr;
	h->remote_connection_len = sockaddr_len;
	h->remote_socket = socket_desc;
	h->first = true;
	h->tfo = true;

	return h;
error:
	close(socket_desc);
	if (h != NULL){
		delete h;
	}
	return NULL;
}

// add a check in the worker
bool check_add(hck_handle* hck, struct addrinfo addr, struct sockaddr_storage sockaddr, time_t now, int source, bool tfo = true){
	struct hck_details* h;

	h = keepalive_lookup(hck, addr.ai_addrlen, sockaddr, now, source);

	if (h == NULL) {
		h = create_new_hck(hck, addr.ai_addrlen, sockaddr, now, source, tfo);

		if (h != NULL) {
			//Assert that socket entries are cleaned up when sockets are closed
			assert(hck->sockets.find(h->remote_socket) == hck->sockets.end());
			assert(h->client_socket == source);
			hck->sockets[h->remote_socket] = h;
			return true;
		}
	}
	else
	{
		assert(hck->sockets[h->remote_socket] == h);
		assert(h->client_socket == source);
		return true;
	}

	return false;
}

static void http_cleanup(hck_handle& hck, struct hck_details* h){
	int erased;

	if (h->state == hck_details::keepalive){
		//Assert that the DB is in the correct state
		assert(hck.keepalived.find(h->remote_connection) != hck.keepalived.end());

		//Clear the keepalive
		erased = hck.keepalived.erase(h->remote_connection);
		assert(erased == 1);
	}

	//Cleanup remote
	if (h->remote_socket != -1){
		erased = hck.sockets.erase(h->remote_socket);
		assert(erased == 1);
		shutdown(h->remote_socket, SHUT_RDWR);
		close(h->remote_socket);
	}


	linger lin;
	unsigned int y = sizeof(lin);
	lin.l_onoff = 1;
	lin.l_linger = 10;
	setsockopt(h->client_socket, SOL_SOCKET, SO_LINGER, (void*)(&lin), y);

	//Close client socket
	shutdown(h->client_socket, SHUT_RDWR);
	close(h->client_socket);

	//Finally free memory
	delete h;
}

// handle a http event
void handle_http(hck_handle& hck, struct epoll_event e, time_t now){
	int rc;
	struct hck_details* h;
	char respbuff[READSIZE];

	h = hck.sockets[e.data.fd];

	if (h->state == hck_details::connecting){
		if (e.events & EPOLLIN || e.events & EPOLLOUT){
			/* Connection success */
			e.events = EPOLLOUT;
			rc = epoll_ctl(hck.epfd, EPOLL_CTL_MOD, e.data.fd, &e);
			if (rc < 0)
			{
				hck_log(LOG_LEVEL_WARNING, "Unable to mod epoll: %s", strerror(errno));
			}
			h->state = hck_details::writing;
		}
		else{
			/* Failed to connect */
			if (h->tfo){
				/* Attempt to re-connect without TFO */
				h->tfo = false;

				int erased = hck.sockets.erase(e.data.fd);
				assert(erased == 1);

				close(h->remote_socket);
				h->remote_socket = create_new_socket(h->remote_connection_len, h->remote_connection, false);
				if (h->remote_socket == -1){
					goto send_failure;
				}
				else{
					hck.sockets[h->remote_socket] = h;
				}

				return;
			}
		}
	}


	/* Do not pass go, do not collect $200 */
	/* An error has occured on the socket, time to cleanup */
	if (e.events & EPOLLERR){
		if (h->state == hck_details::keepalive){
			hck_log(LOG_LEVEL_WARNING, "Closing HTTP keepalive connection due to error");
			http_cleanup(hck, h);
		}
		else{
			recv(e.data.fd, 0, 0, 0);
			hck_log(LOG_LEVEL_WARNING, "Sending failure due to error: %s", strerror(errno));
			goto send_failure;
		}
		return;
	}

	if (h->state == hck_details::writing){
		rc = send(e.data.fd, http_request + h->position, http_request_size - h->position, 0);
		if (rc == -1){
			if (errno == EAGAIN || errno == EWOULDBLOCK){
				return;
			}
			hck_log(LOG_LEVEL_WARNING, "HCK: failed to send data (%s)\n", strerror(errno));
			if (!h->first){
				goto send_retry;
			}
			goto send_failure;
		}
		h->position += rc;
		if (h->position == http_request_size){
			h->state = hck_details::reading1;
			h->position = 0;

			e.events = EPOLLIN;
			rc = epoll_ctl(hck.epfd, EPOLL_CTL_MOD, e.data.fd, &e);
			if (rc < 0)
			{
				hck_log(LOG_LEVEL_WARNING, "epoll mod error: %s", strerror(errno));
			}
		}
	}
	else if (h->state == hck_details::reading1){
		int i = http_resp_startlen - h->position;
		rc = recv(e.data.fd, respbuff, sizeof(respbuff), 0);

		if (rc  == -1){
			if (errno == EAGAIN || errno == EWOULDBLOCK){
				return;
			}
			hck_log(LOG_LEVEL_WARNING, "HCK: failed to recv data (%s)\n", strerror(errno));
			if (!h->first && h->position == 0){
				goto send_retry;
			}
			goto send_failure;
		}

		if (rc > i){
			i -= 1;
			if (respbuff[i] > '0' && respbuff[i] < '5'){
				uint8_t nls = 0;
				//minimum of 2 places, this char and space
				//4 would be standards compliant for "200 "
				for (i+=2; i < rc; i++){
					if (respbuff[i] == '\n'){
						if (++nls == 2){
							goto send_ok;
						}
					}
					else if (respbuff[i] != '\r'){
						nls = 0;
					}
				}

				//Position signifies number of newlines in reading2
				h->state = hck_details::reading2;
				h->position = nls;
			}
			else{
				hck_log(LOG_LEVEL_WARNING, "HCK: invalid response (char: %d)\n", respbuff[i] - '0');
				goto send_failure;
			}
		}
		else {
			h->position += rc;
		}
	}


	if (h->state == hck_details::reading2){
		rc = recv(e.data.fd, respbuff, sizeof(respbuff), 0);
		uint8_t nls = h->position;
		for (int i = 0; i < rc; i++){
			if (respbuff[i] == '\n'){
				if (++nls == 2){
					goto send_ok;
				}
			}
			else if (respbuff[i] != '\r'){
				nls = 0;
			}
		}
		h->position = nls;
	}
	else if (h->state == hck_details::keepalive){
		rc = recv(e.data.fd, respbuff, sizeof(respbuff), 0);
		if (rc == -1){
			hck_log(LOG_LEVEL_WARNING, "Keepalive connection closing, no longer open");
			http_cleanup(hck, h);
			return;
		}
	}
	else if (h->state == hck_details::recovery){
		if (e.events & EPOLLHUP || e.events & EPOLLRDHUP || e.events & EPOLLERR){
			hck_log(LOG_LEVEL_WARNING, "Keepalive recovery connection closing, no longer open");
			h->expires = 0;
			http_cleanup(hck, h);
			return;
		}

		// Place back into wiritng
		h->state = hck_details::writing;
		assert(h->position == 0);
	}

	if (e.events & EPOLLOUT == 0 && e.events & EPOLLIN == 0 && (e.events & EPOLLHUP || e.events & EPOLLRDHUP)){
		if (h->state == hck_details::keepalive){
			hck_log(LOG_LEVEL_WARNING, "Keepalive connection closed");
			http_cleanup(hck, h);
			return;
		}
		else{
			hck_log(LOG_LEVEL_WARNING, "HCK: connection interrupted\n");
			goto send_failure;
		}
	}

	return;

send_ok:
	if (!send_result(&hck, h->client_socket, 1)){
		hck_log(LOG_LEVEL_WARNING, "Failed to send ok: %s", strerror(errno));
		http_cleanup(hck, h);
		return;
	}
	else{
		h->position = 0;
		h->state = hck_details::keepalive;
		h->expires = now + TIMEOUT_POST;

		/* If a keepalive already exists, don't re-add */
		if (hck.keepalived.find(h->remote_connection) != hck.keepalived.end()) {
			assert(hck.keepalived[h->remote_connection] != h->remote_socket);
			hck_log(LOG_LEVEL_WARNING, "Extra connection was opened, no longer needed - a keepalived connection exists.");
			http_cleanup(hck, h);
		}
		else 
		{
			hck.keepalived[h->remote_connection] = h->remote_socket;

			//Only get read events for keepalive
			e.events = EPOLLIN;
			rc = epoll_ctl(hck.epfd, EPOLL_CTL_MOD, e.data.fd, &e);
		}
	}
	return;
send_failure:
	if (h->state != hck_details::keepalive){
		send_result(&hck, h->client_socket, 0);
	}
	http_cleanup(hck, h);
	return;
send_retry:
	send_result(&hck, h->client_socket, 3);
	http_cleanup(hck, h);
	return;
}

// handle internal communication
void handle_internalsock(hck_handle& hck, int socket, time_t now){
	struct addrinfo servinfo;
	sockaddr_storage s = { 0 };
	int rc;

	//read addr
	int required = sizeof(servinfo);
	void* ptr = &servinfo;
	do {
		rc = recv(socket, ptr, required, MSG_WAITALL);
		if (rc == -1 || rc == 0){
			close(socket);
			return;
		}
		ptr += rc;
		required -= rc;
	} while (required);
	
	//read sockaddr
	required = servinfo.ai_addrlen;
	ptr = &s;
	do {
		rc = recv(socket, ptr, required, MSG_WAITALL);
		if (rc == -1 || rc == 0) {
			close(socket);
			return;
		}
		ptr += rc;
		required -= rc;
	} while (required);
	
	if (!check_add(&hck, servinfo, s, now, socket)) {
		//close on error
		close(socket);
	}
}

void handle_cleanup(hck_handle& hck, time_t now){
	struct hck_details* h;
	std::vector<int> to_delete;

	for (map<int, struct hck_details*>::iterator it = hck.sockets.begin(); it != hck.sockets.end(); it++){
		h = it->second;
		if (h->expires < now){
			to_delete.push_back(it->first);

			hck_log(LOG_LEVEL_WARNING, "Expiring socket %d in state %d", h->remote_socket, h->state);
		}
	}
	for (std::vector<int>::iterator it = to_delete.begin(); it != to_delete.end(); it++){
		int idx = *it;
		h = hck.sockets[*it];

		if (h->state != hck_details::keepalive){
			send_result(&hck, h->client_socket, false);
		}
		
		int erased = hck.sockets.erase(idx);
		assert(erased == 1);

		close(h->client_socket);
		close(h->remote_socket);
		delete h;
	}
}

int create_listener(){
	int fd;
	struct sockaddr_un addr;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket error");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	hck_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	unlink(socket_path);

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		hck_log(LOG_LEVEL_WARNING, "bind error (%d): Unable to bind to \\0%s", errno, socket_path+1);
		return -1;
	}

	if (listen(fd, 5) == -1) {
		perror("listen error");
		return -1;
	}

	return fd;
}

bool set_blocking_mode(const int &socket, bool is_blocking)
{
	bool ret = true;

	const int flags = fcntl(socket, F_GETFL, 0);
	ret = 0 == fcntl(socket, F_SETFL, is_blocking ? (flags ^ O_NONBLOCK) : (flags | O_NONBLOCK));	

	return ret;
}

/*
Main loop for processing check requests
*/
void main_thread(){
	int n;
	hck_handle hck;
	time_t now = 0;
	time_t lasttime = 0;
	int fd;

	struct epoll_event events[MAXEVENTS];
	struct epoll_event e = { 0 };
	struct hck_details* h;

	hck.epfd = epoll_create(1024);
	localtime(&now);
	
	/* Create internal listener */
	fd = create_listener();
	if (fd == -1){
		close(hck.epfd);
		return;
	}

	/* Add the listener to EPOLL */
	e.data.fd = fd;
	e.events = EPOLLIN;
	epoll_ctl(hck.epfd, EPOLL_CTL_ADD, fd, &e);

	hck_log(LOG_LEVEL_WARNING, "Zabbix HCK Main thread started on \\0%s", socket_path+1);

	assert(running);
	while (running){
		/* Update timestamp once per loop */
		time(&now);

		n = epoll_wait(hck.epfd, events, MAXEVENTS, 100);
		while (n > 0){
			n--;

			e = events[n];

			if (hck.sockets.find(e.data.fd) != hck.sockets.end()){ /* handle events for the checks */
				handle_http(hck, e, now);
			}
			else if (e.data.fd == fd){ 
				/* Handle new connections to the main thread */
				if (e.events & EPOLLIN){
					/* Accept & Add to EPOLL */
					e.data.fd = accept(e.data.fd, 0, 0);
					if (e.data.fd == -1){
						hck_log(LOG_LEVEL_WARNING, "Unable to accept internal communication socket: %s", strerror(errno));
						continue;
					}
					set_blocking_mode(e.data.fd, true);
					epoll_ctl(hck.epfd, EPOLL_CTL_ADD, e.data.fd, &e);
				}
				else{
					goto cleanup;
					return;
				}
			}
			else{ /* handle events for a connection to the main thread */
				if (e.events & EPOLLIN){
					handle_internalsock(hck, e.data.fd, now);
				}
				else if (e.events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
					hck_log(LOG_LEVEL_WARNING, "An error occured with client socket %d. Closing", e.data.fd);

					//error
					bool found = false;
					for (map<int, struct hck_details*>::iterator it = hck.sockets.begin(); it != hck.sockets.end(); it++){
						struct hck_details* h = it->second;
						if (h->client_socket == e.data.fd){
							assert(!found);//todo: add if debug break
							h->client_socket = -1;
							found = true;
						}
					}

					/* is it not a client socket? */
					if (!found){
						hck_log(LOG_LEVEL_WARNING, "HCK: closing socket %d of unknown type\n", e.data.fd);
					}

					close(e.data.fd);
				}
			}
		}

		if (now > lasttime){
			lasttime = now;
			handle_cleanup(hck, now);
		}
	}

cleanup:
	hck_log(LOG_LEVEL_WARNING, "Zabbix HCK cleanup");

	close(fd);
	close(hck.epfd);

	//todo: remote socket & keepalive
	for (map<int, struct hck_details*>::iterator it = hck.sockets.begin(); it != hck.sockets.end(); it++){
		close(it->second->client_socket);
		delete it->second;
	}
}

unsigned short execute_check(int fd, const char* addr, const char* port, bool retry){
	int rc;
	unsigned short result;
	struct addrinfo hints;
	struct addrinfo *servinfo = NULL;  // will point to the results

	memset(&hints, 0, sizeof hints); // make sure the struct is empty

	hints.ai_family = AF_UNSPEC;     // don't care IPv4 or IPv6
	hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
	hints.ai_flags = AI_PASSIVE;     // fill in my IP for me

	if ((rc = getaddrinfo(addr, port, &hints, &servinfo)) != 0) {
		perror("get addr info failed");
		return 4;
	}
	
	rc = send(fd, (void*)servinfo, sizeof(addrinfo), 0);
	if (rc < 0) {
		freeaddrinfo(servinfo); // free the linked-list
		perror("io error during send (2)");
		return 4;
	}
	freeaddrinfo(servinfo); // free the linked-list

	rc = send(fd, (void*)servinfo->ai_addr, servinfo->ai_addrlen, 0);
	if (rc < 0){
		perror("io error during send (1)");
		return 4;
	}

	int required = sizeof(result);
	void* ptr = &result;
	do {
		rc = recv(fd, ptr, required, MSG_WAITALL);
		if (rc == 0){
			perror("socket shutdown, no more data");
			return 4;
		}
		if (rc == -1){
			perror("io error during recv");
			return 4;
		}
		required -= rc;
		ptr += rc;
	} while (required);

	if (result == 3){
		if (!retry){
			return 0;
		}

		//retry
		return execute_check(fd, addr, port, false);
	}

	return result;
}

int connect_to_hck(){
	struct sockaddr_un addr;
	int fd;

	//create new unix socket
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket error");
		return -1;
	}

	//setup unix socket address
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	hck_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	//connect to unix socket
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		perror("connect error");
		close(fd);
		return -1;
	}

	return fd;
}

void handle_sighup(int signal){
	hck_log(LOG_LEVEL_DEBUG, "Sighup received");
	running = 0;
}

void processing_thread(){
	// Setup the sighup handler
	struct sigaction sa;
	sa.sa_handler = &handle_sighup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);

	// Intercept SIGHUP
	if (sigaction(SIGHUP, &sa, NULL) == -1) {
		perror("Error: cannot handle SIGHUP"); // Should not happen
	}

	// Send SIGHUP if parent exits
	prctl(PR_SET_PDEATHSIG, SIGHUP);

	// Run until then
	while (running){
		main_thread();
	}
}