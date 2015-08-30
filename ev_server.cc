#include <event.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

#include <queue>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstring>

#define THREAD_NUM 12
#define MAX_CONNS 1024
#define BACKLOG 1024
#define DATA_BUFFER_SIZE 2048

enum conn_states {
    conn_new_cmd,    /**< Prepare connection for next command */
    conn_waiting,    /**< waiting for a readable socket */
    conn_read,       /**< reading in a command line */
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      /**< writing out a simple response */
    conn_nread,      /**< reading in a fixed number of bytes */
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    /**< closing this connection */
    conn_mwrite,     /**< writing out many items sequentially */
    conn_closed,     /**< connection is closed */
    conn_max_state   /**< Max state value (used for assertion) */
};

enum try_read_result {
	READ_DATA_RECEIVED,
	READ_NO_DATA_RECEIVED,
	READ_ERROR,            /** an error occurred (on the socket) (or client closed connection) */
	READ_MEMORY_ERROR      /** failed to allocate more memory */
};

class cq_item {
public:
	int sfd;
	enum conn_states init_state;
	int event_flags;
};

class conn_queue {
private:
	std::queue<cq_item*> q;
	pthread_mutex_t lock;
public:
	conn_queue() {
		pthread_mutex_init(&lock, NULL);
	}

	void push(cq_item* item) {
		pthread_mutex_lock(&lock);
		q.push(item);	
		pthread_mutex_unlock(&lock);
	}

	cq_item* pop() {
		cq_item* c = NULL;
		pthread_mutex_lock(&lock);
		if (!q.empty()) {
			c = q.front();
			q.pop();
		}
		pthread_mutex_unlock(&lock);
		return c;
	}
};

struct thread {
	pthread_t pid;
	struct event_base *base;
	struct event notify_event;
	
	int notify_receive_fd;
	int notify_send_fd;
	
	conn_queue queue;
};

class conn {
public:
	struct event_base* base;
	struct event event;
	int sfd;
	short which;
	enum conn_states state;
	struct thread* thread;
	int ev_flags;

	char* rbuf;
	char* rcurr;
	int rsize;
	int rbytes;

    char *wbuf;
    char *wcurr;
    int wsize;
    int wbytes;

	void set_state(enum conn_states state) {
		this->state = state;
	}

	int try_read_command() {
		char* el;
		char* cont;
		
		if (rbytes == 0) {
			return 0;
		}

		el = (char*) memchr(rcurr, '\n', rbytes);

		return 0;	
	}

	enum try_read_result try_read_network() {
		enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
		int buf[255];
		int n;
		do {
			n = read(sfd, buf, 255);
			if (n > 0) {
				printf("%s\n", buf);
			}
		} while (n > 0);
		return gotdata;
	}

	bool update_event(const int new_flags) {
		struct event_base *base = event.ev_base;
		if (ev_flags == new_flags) {
			return true;
		}
		if (event_del(&event) == -1) {
			return false;
		}
		event_set(&event, sfd, new_flags, event_handler, (void *) this);
		event_base_set(base, &event);
		ev_flags = new_flags;
		if (event_add(&event, 0) == -1) {
			return false;
		}
		return true;
	}

	static void event_handler(const int fd, const short which, void* arg) {
		conn *c;
		c = (conn*) arg;
		assert(c != NULL);

		c->which = which;
		
		if (fd != c->sfd) {
			conn_close(c);
			return;
		}

		bool stop = false;
		while (!stop) {
			switch (c->state) {
				case conn_new_cmd:
					c->set_state(conn_parse_cmd);		
					break;
				case conn_parse_cmd:
					if (c->try_read_command() == 0) {
						c->set_state(conn_waiting);
					}
					break;
				case conn_waiting:
					if (!c->update_event(EV_READ | EV_PERSIST)) {
						c->set_state(conn_closing);
					}
					c->set_state(conn_read);
					stop = true;
					break;
				case conn_read:
					c->try_read_network();	
					break;
				case conn_closing:
					conn::conn_close(c);
					stop = true;
					break;
			}
			break;
		}
	}

	static conn* conn_new(const int sfd, enum conn_states init_state, const int event_flags, struct event_base* base) {
		conn* c;
		c = conns[sfd];
		if (c == NULL) {
			c = new conn();
			
			c->base = base;
			
			c->rbuf = c->wbuf = 0;
			
			c->rsize = DATA_BUFFER_SIZE;
			c->wsize = DATA_BUFFER_SIZE;

			c->rbuf = new char[c->rsize];
			c->wbuf = new char[c->wsize];

			c->sfd = sfd;
			conns[sfd] = c;
		}

		c->state = init_state;
		c->rbytes = c->wbytes = 0;
		c->wcurr = c->wbuf;
		c->rcurr = c->rbuf;

		event_set(&c->event, sfd, event_flags, event_handler, (void*)c);
		event_base_set(base, &c->event);
		c->ev_flags = event_flags;
		
		event_add(&c->event, 0);
		return c;
	}

	static void conn_close(conn* c) {
		assert(c != NULL);
		event_del(&c->event);

		close(c->sfd);
	}
	static conn** conns;
private:
	conn() {
	}
};
conn** conn::conns = new conn*[MAX_CONNS];


static struct thread* threads;
static conn_queue queue;
static struct event_base *main_base;
static int last_thread = -1;
static int init_count = 0;

static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;
static pthread_mutex_t worker_hang_lock;

void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
    
	pthread_mutex_lock(&worker_hang_lock);
    pthread_mutex_unlock(&worker_hang_lock);
}

void thread_libevent_process(int sock, short which, void* arg) {
	struct thread* me = (struct thread*) arg;
	cq_item* item;
	char buf[1];

	read(sock, buf, 1);
	printf("%c\n", buf[0]);
	switch (buf[0]) {
		case 'c':
		item = me->queue.pop();
		if (item != NULL) {
			conn* c = conn::conn_new(item->sfd, item->init_state, item->event_flags, me->base);
			if (c != NULL) {
				c->thread = me;
			}
			delete item;
		}
	}
}

void setup_thread(struct thread* me) {
	me->base = event_init();

	event_set(&me->notify_event, me->notify_receive_fd, EV_READ | EV_PERSIST, thread_libevent_process, me);
	event_base_set(me->base, &me->notify_event);
	event_add(&me->notify_event, 0);
	
	register_thread_initialized();
}

void* worker_libevent(void* arg) {
	struct thread* me = (struct thread*) arg;
	event_base_loop(me->base, 0);

	return NULL;
}

void thread_init(int nthread, struct event_base* base) {
    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);
	pthread_mutex_init(&worker_hang_lock, NULL);

	threads = new thread[nthread];

	for (int i = 0; i < nthread; i++) {
		int fds[2];
		pipe(fds);
		
		threads[i].notify_receive_fd = fds[0];
		threads[i].notify_send_fd = fds[1];

		setup_thread(&threads[i]);
	}

	for (int i = 0; i < nthread; i++) {
		pthread_create(&threads[i].pid, NULL, worker_libevent, &threads[i]);
	}

    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthread);
    pthread_mutex_unlock(&init_lock);
}

int server_socket(const char* interface, int port) {
	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(interface);

	bind(sfd, (struct sockaddr*) &addr, sizeof(struct sockaddr));
	listen(sfd, BACKLOG);
	return sfd;
}

void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags) {
    int tid = (last_thread + 1) % THREAD_NUM;
    struct thread* thread = threads + tid;
    last_thread = tid;
	
	cq_item* item = new cq_item();
	
	item->sfd = sfd;
	item->init_state = init_state;
	item->event_flags = event_flags;
	
	thread->queue.push(item);

	char buf[1];
	buf[0] = 'c';
	write(thread->notify_send_fd, buf, 1); 
}

void base_event_handler(int sock, short event, void* arg) {
    struct sockaddr_in cli_addr;
    int sfd;
    socklen_t sin_size;
    sin_size = sizeof(struct sockaddr_in);
    sfd = accept(sock, (struct sockaddr*)&cli_addr, &sin_size);

	dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST);
}

int main() {
	main_base = event_init();
	thread_init(THREAD_NUM, main_base);
	int sfd = server_socket("127.0.0.1", 11212);
	struct event listen_ev;
	event_set(&listen_ev, sfd, EV_READ | EV_PERSIST, base_event_handler, NULL);
	event_base_set(main_base, &listen_ev);
	event_add(&listen_ev, NULL);

	event_base_loop(main_base, 0); 
	return 0;
}
