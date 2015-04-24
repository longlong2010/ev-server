#include <event.h>
#include <pthread.h>
#include <unistd.h>
#include <queue>

#define THREAD_NUM 12

struct thread {
	pthread_t pid;
	struct event_base *base;
	struct event notify_event;
	
	int notify_receive_fd;
	int notify_send_fd;
};

class conn {

};

class conn_queue {
private:
	std::queue<conn*> q;
	pthread_mutex_t lock;
public:
	conn_queue() {
		pthread_mutex_init(&lock, NULL);
	}

	void push(conn* c) {
		pthread_mutex_lock(&lock);
		q.push(c);	
		pthread_mutex_unlock(&lock);
	}

	conn* pop() {
		conn* c = NULL;
		pthread_mutex_lock(&lock);
		if (!q.empty()) {
			c = q.front();
			q.pop();
		}
		pthread_mutex_unlock(&lock);
		return c;
	}
};

static thread* threads;
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

void thread_libevent_process(int sock, short event, void* arg) {
	thread* me = (thread*) arg;
	char buf[1];
	read(sock, buf, 1);
	printf("%s\n", buf);
}

void setup_thread(thread* me) {
	me->base = event_init();

	event_set(&me->notify_event, me->notify_receive_fd, EV_READ | EV_PERSIST, thread_libevent_process, me);
	event_base_set(me->base, &me->notify_event);
	event_add(&me->notify_event, 0);
	
	register_thread_initialized();
}

void* worker_libevent(void* arg) {
	thread* me = (thread*) arg;
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
	addr.sin_addr.s_addr = INADDR_ANY;

	bind(sfd, (struct sockaddr*) &addr, sizeof(struct sockaddr));
	listen(sfd, 1024);
	return sfd;
}

void base_event_handler(int sock, short event, void* arg) {
    struct sockaddr_in cli_addr;
    int newfd;
    socklen_t sin_size;
    sin_size = sizeof(struct sockaddr_in);
    newfd = accept(sock, (struct sockaddr*)&cli_addr, &sin_size);

    int tid = (last_thread + 1) % THREAD_NUM;
    thread* thread = threads + tid;
    last_thread = tid;
    
	write(thread->notify_send_fd, "K", 1); 
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
