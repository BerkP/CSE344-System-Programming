#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>


#define TRUE 			1
#define FALSE 			0
#define BUFFER_LEN 		256
#define ENTRY_SIZE 		128
#define BD_MAX_CLOSE 	8192
#define EXCL_NAME 		"prevent"

struct column_t{
	char **entries;
};
typedef struct column_t column_t;


struct database_t{
	column_t *columns;
	int columnsize;
	int rowsize;
};
typedef struct database_t database_t;


struct SocketQueue{
	struct QueueNode *head;
	struct QueueNode *tail;
};
typedef struct SocketQueue SocketQueue;


struct QueueNode{
	char socketfd;
	struct QueueNode *next;
};
typedef struct QueueNode QueueNode;


struct thread_info{
	int number;
	pthread_t tid;
	int thread_created;
};
typedef struct thread_info thread_info;


void *pool_thread_func(void *);
int give_response(char* , int );
int update_q(char **query, int size, int clientfd);
int get_next_index_from_column(column_t *column, int currindex, char *value);
int select_q(char **query, int size, int clientfd);
int get_column_index(char *columnname);
char** parse_query(char *query, int *size);
int send_table(column_t *cols, int size, int clientfd);
int create_pool_threads(int );
int create_pool_arr(int );
int parse_command_line_opt(int , char **, char** , char** , int* , int* );
int is_str_a_int(char *);
void write_log(char *);
void free_string_array(char** arr, int size);
int init_sigint_handling(sigset_t *oldmask);
void join_all_threads(int size);
void clean_up();
//Queue functions
int init_socket_queue();
int add_socket_to_queue(int );
int get_socket_from_queue(int *);
void free_queue();
//DB functions
int create_db(FILE *);
int fill_db(FILE *);
void free_db();
//Reader writer functions
void get_reader_lock();
void release_reader_lock();
void get_writer_lock();
void release_writer_lock();

//sync log print
pthread_mutex_t pmutex = PTHREAD_MUTEX_INITIALIZER;
//between main and pool threads
pthread_mutex_t thmutex = PTHREAD_MUTEX_INITIALIZER;
//sync for queue
pthread_mutex_t qmutex = PTHREAD_MUTEX_INITIALIZER;
//sync for reader writer
pthread_mutex_t rwmutex = PTHREAD_MUTEX_INITIALIZER;

//cond variable for start barrier
pthread_cond_t startbarrier = PTHREAD_COND_INITIALIZER;
//thread managing cond variables
pthread_cond_t full = PTHREAD_COND_INITIALIZER;
pthread_cond_t empty = PTHREAD_COND_INITIALIZER;
//reader writer cond variable
pthread_cond_t okToRead = PTHREAD_COND_INITIALIZER;
pthread_cond_t okToWrite = PTHREAD_COND_INITIALIZER;


database_t db;
thread_info *pool_threads = NULL;
SocketQueue *queue = NULL;
FILE *logfp = NULL;
FILE* fp = NULL;
sem_t *temp_sem;

int busythread = 0;
int socketcount = 0;
int initedthreads = 0;
int term_flag = FALSE;

//reader-writer state variables
int AR = 0;
int AW = 0;
int WR = 0;
int WW = 0;


void int_handler(int signo){
	char log[BUFFER_LEN];
	sprintf(log, "Termination signal received, waiting for ongoing threads to complete.");
	write_log(log);
	term_flag = TRUE;

}

int become_daemon(int flag){
	int maxfd, fd;

	switch(fork()){
		case -1	: return -1;
		case 0	: break;
		default : exit(EXIT_SUCCESS);

	}

	if(setsid() == -1){
		return -1;
	}

	switch(fork()){
		case -1	: return -1;
		case 0	: break;
		default : exit(EXIT_SUCCESS);
	}

	if(flag == 0){
		umask(0);
	}

	maxfd = sysconf(_SC_OPEN_MAX);
	if (maxfd == -1){
		maxfd = BD_MAX_CLOSE;
	}

	if(flag == 0){
		for(fd = 0; fd < maxfd; fd++){
			close(fd);
		}
	}
	

	return 0;

}

int main(int argc, char **argv){
	char log[BUFFER_LEN];
	char *logfilename;
	char *datafilename;
	int port, poolsize, socketfd, tempfp, error;
    struct sockaddr_in server_a;
    struct sockaddr_in temp_a;
    int option_value = 1;
    struct timeval begin, end;
    double loadtime;
    sigset_t oldmask;
    


    if( (temp_sem = sem_open(EXCL_NAME, O_CREAT | O_EXCL, 0666, 0)) == SEM_FAILED ){
    	fprintf(stderr,"Server is already running! - %s\n", strerror(errno));
		exit(EXIT_FAILURE);
    }

	if(parse_command_line_opt(argc, argv, &logfilename, &datafilename, &port, &poolsize) == -1){
		fprintf(stderr,"Correct usage:\n\t./server -p PORT(integer) -o pathToLogFile –l poolSize(integer>=2) –d datasetPath\n");
		clean_up();
		exit(EXIT_FAILURE);
	}


	if(become_daemon(0) == -1){
		fprintf(stderr,"Daemon error: - %s\n", strerror(errno));
		clean_up();
		exit(EXIT_FAILURE);
	}

	if( (logfp = fopen(logfilename, "w")) == NULL){
		clean_up();
		exit(EXIT_FAILURE);
	}

	sprintf(log, "Executing with parameters:");
	write_log(log);
	sprintf(log, "-p %d", port);
	write_log(log);
	sprintf(log, "-o %s", logfilename);
	write_log(log);
	sprintf(log, "-l %d", poolsize);
	write_log(log);
	sprintf(log, "-d %s", datafilename);
	write_log(log);


	if(init_sigint_handling(&oldmask) == -1){
		sprintf(log, "Signal handling error: %s\n", strerror(errno));
		write_log(log);
		clean_up();
		exit(EXIT_FAILURE);
	}

	if( (fp = fopen(datafilename, "r")) == NULL){
		sprintf(log, "File error: %s\n", strerror(errno));
		write_log(log);
		clean_up();
		exit(EXIT_FAILURE);
	}

	sprintf(log, "Loading dataset...");
	write_log(log);
	gettimeofday(&begin, NULL);

	if(create_db(fp) == -1 ){
		clean_up();
		exit(EXIT_FAILURE);
	}

	if(fill_db(fp) == -1){
		clean_up();
		exit(EXIT_FAILURE);
	}

	gettimeofday(&end, NULL);
	loadtime = (double)((((end.tv_sec - begin.tv_sec) * 1000000) + end.tv_usec) - (begin.tv_usec)) / 1000000;
	sprintf(log, "Dataset loaded in %lf seconds with %d records.", loadtime, db.rowsize-1);
	write_log(log);


	if(create_pool_arr(poolsize) == -1){
		clean_up();
		exit(EXIT_FAILURE);
	}

	pthread_mutex_lock(&thmutex);
	if(create_pool_threads(poolsize) == -1){
		clean_up();
		exit(EXIT_FAILURE);
	}

	sprintf(log, "A pool of %d threads has been created", poolsize);
	write_log(log);
	pthread_mutex_unlock(&thmutex);

	if( (error = pthread_sigmask(SIG_SETMASK, &oldmask, NULL)) ){
		sprintf(log, "Failed to restore sigint mask: %s", strerror(error));
		write_log(log);
		clean_up();
		exit(EXIT_FAILURE);
	}

	pthread_mutex_lock(&thmutex);
	while(initedthreads != poolsize){
		pthread_cond_wait(&startbarrier, &thmutex);
	}
	pthread_mutex_unlock(&thmutex);
	

	if(init_socket_queue() == -1){
		clean_up();
		exit(EXIT_FAILURE);
	}

	socketfd = -1;
    if((socketfd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
    	sprintf(log, "Socket error - %s  \n", strerror(errno));
		write_log(log);
		clean_up();
		exit(EXIT_FAILURE);
    }

    if(setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(int)) == -1){
    	sprintf(log, "setsockopt error - %s  \n", strerror(errno));
		write_log(log);
		clean_up();
		exit(EXIT_FAILURE);
    }

    server_a.sin_family = AF_INET;
    server_a.sin_port = htons(port);
    server_a.sin_addr.s_addr = INADDR_ANY;

    if(bind(socketfd, (struct sockaddr *)&server_a, sizeof(struct sockaddr)) == -1){
    	sprintf(log, "bind error - %s  \n", strerror(errno));
		write_log(log);
		clean_up();
		exit(EXIT_FAILURE);
    }

    if(listen(socketfd, 100) == -1){
    	sprintf(log, "listen error - %s  \n", strerror(errno));
		write_log(log);
		clean_up();
		exit(EXIT_FAILURE);
    }
	
	while(!term_flag){

		socklen_t len = sizeof(struct sockaddr_in);
    	if((tempfp = accept(socketfd, (struct sockaddr*)&temp_a, &len)) == -1 && !term_flag){
    		sprintf(log, "accept error - %s  \n", strerror(errno));
			write_log(log);
			clean_up();
			exit(EXIT_FAILURE);
    	}
    	if(term_flag)	break;


    	pthread_mutex_lock(&qmutex);
    	if (add_socket_to_queue(tempfp) == -1){
			exit(EXIT_FAILURE);	
		}
    	pthread_mutex_unlock(&qmutex);

		pthread_mutex_lock(&thmutex);

		while(busythread == poolsize && !term_flag){
			sprintf(log, "No thread is available! Waiting…");
			write_log(log);
			pthread_cond_wait(&empty, &thmutex);
		}

		busythread++;
		socketcount++;
		pthread_cond_broadcast(&full);
		pthread_mutex_unlock(&thmutex);
	}


	pthread_cond_broadcast(&full);
	join_all_threads(poolsize);

	if(socketfd != -1){
		shutdown(socketfd, SHUT_RD);
	}
	close(socketfd);
	fclose(fp);
	free_db();
	free_queue();
	free(pool_threads);

	sprintf(log, "All threads have terminated, server shutting down. ");
	write_log(log);
	fclose(logfp);
	sem_close(temp_sem);
	sem_unlink(EXCL_NAME);
	
	return 0;

}


void *pool_thread_func(void *arg){

	char log[BUFFER_LEN];
	char buffer[BUFFER_LEN];
	int number;
	int clientfd;
	int socketdone;
	int res;

	number = *((int *)(arg));


	pthread_mutex_lock(&thmutex);
	sprintf(log, "Thread #%d: waiting for connection", number);
	write_log(log);
	initedthreads++;
	pthread_mutex_unlock(&thmutex);
	pthread_cond_signal(&startbarrier);


	while(!term_flag){

		pthread_mutex_lock(&thmutex);
		while(socketcount == 0 && !term_flag){
			pthread_cond_wait(&full, &thmutex);
		}
		if(term_flag){
			pthread_mutex_unlock(&thmutex);
			return NULL;
		}

		socketcount--;
		pthread_mutex_lock(&qmutex);
    	if (get_socket_from_queue(&clientfd) == -1 || term_flag){
    		pthread_mutex_unlock(&qmutex);
			pthread_mutex_unlock(&thmutex);
			return NULL;
		}
		sprintf(log, "A connection has been delegated to thread id #%d", number);
		write_log(log);
    	pthread_mutex_unlock(&qmutex);
		pthread_mutex_unlock(&thmutex);

		socketdone = FALSE;
		while(!socketdone){
			read(clientfd, buffer, BUFFER_LEN);

			if(strcmp(buffer,"end") == 0){
				socketdone = TRUE;
			}
			else{
				sprintf(log, "Thread #%d: received query'%s'", number, buffer);
				write_log(log);

				if((res = give_response(buffer, clientfd)) == -1){
					socketdone = TRUE;
					sprintf(log, "Thread #%d: Something went wrong with query. Current errno: %s", number, strerror(errno));
					write_log(log);
				}
				else{
					sprintf(log, "Thread #%d: query completed, %d records have been returned.", number, res);
					write_log(log);
				}
			}
		}

		if(term_flag){
			pthread_cond_signal(&empty);
			return NULL;
		}

		close(clientfd);

		pthread_mutex_lock(&thmutex);
		sprintf(log, "Thread #%d: waiting for connection", number);
		write_log(log);
		busythread--;
		pthread_cond_signal(&empty);
		pthread_mutex_unlock(&thmutex);

	}
	return NULL;

}


int give_response(char* query, int clientfd){
	int size;
	char **parsedquery;
	int res;
	struct timespec requested_time = { (time_t)0, (long)500000000L };
    struct timespec remaining_time = { (time_t)0, (long)0L };

	parsedquery = parse_query(query, &size);

	if(strcmp(parsedquery[0],"SELECT") == 0){
		get_reader_lock();
		if(strcmp(parsedquery[1],"DISTINCT") == 0){
			res = select_q(&parsedquery[2], size-2, clientfd);
		}
		else{
			res = select_q(&parsedquery[1], size-1, clientfd);
		}
		nanosleep(&requested_time, &remaining_time);
		release_reader_lock();
	}
	else if(strcmp(parsedquery[0],"UPDATE") == 0){
		get_writer_lock();
		res = update_q(&parsedquery[3], size-3, clientfd);
		nanosleep(&requested_time, &remaining_time);
		release_writer_lock();
	}
	else{
		res = -1;
	}

	free_string_array(parsedquery, size);
	return res;

}


int update_q(char **query, int size, int clientfd){
	int i, j, subsize, currindex, temp_index, to_send_cap, to_send_size;
	char* searchcolumn;
	char* searchvalue;
	int* to_send_indexes;
	column_t *column;

	currindex = 0;
	subsize = size -3;
	searchcolumn = query[size-2];
	searchvalue = query[size-1];
	temp_index = get_column_index(searchcolumn);

	if(temp_index == -1){
		return -1;
	}
	column = &db.columns[temp_index];

	to_send_cap = 10;
	to_send_size = 0;
	to_send_indexes = malloc(to_send_cap * sizeof(int));
	if(to_send_indexes == NULL){
		return -1;
	}

	while((currindex = get_next_index_from_column(column, currindex, searchvalue)) != -1){
		to_send_indexes[to_send_size] = currindex;
		to_send_size++;

		if(to_send_size == to_send_cap){
			to_send_cap *= 2;
			to_send_indexes = realloc(to_send_indexes, to_send_cap * sizeof(int));
		}
	}

	for(i = 0; i < subsize; i = i + 2){
		temp_index = get_column_index(query[i]);
		if(temp_index == -1){
			free(to_send_indexes);
			return -1;
		}
		column = &db.columns[temp_index];

		for(j = 0; j < to_send_size; j++){
			strcpy(column->entries[to_send_indexes[j]], query[i+1]);
		}
	}

	temp_index = to_send_size + 1;
	write(clientfd, &db.columnsize, sizeof(int));
	write(clientfd, &temp_index, sizeof(int));

	for(i = 0; i < db.columnsize; i++){
		if(write(clientfd, db.columns[i].entries[0], ENTRY_SIZE) == -1){
			free(to_send_indexes);
			return -1;
		}
		for(j = 0; j < to_send_size; j++){
			if(write(clientfd, db.columns[i].entries[to_send_indexes[j]], ENTRY_SIZE) == -1){
				free(to_send_indexes);
				return -1;
			}
		}
	}
	free(to_send_indexes);
	return to_send_size;

}


int get_next_index_from_column(column_t *column, int currindex, char *value){
	int i=0;

	for(i = currindex+1; i < db.rowsize; i++){
		if(strcmp(value, column->entries[i]) == 0){
			return i;
		}
	}
	return -1;

}


int select_q(char **query, int size, int clientfd){
	column_t *to_send_columns;
	int i;
	int columnindex;
	int sendsize;
	int res;


	if(strcmp(query[0], "*") == 0){
		write(clientfd, &db.columnsize, sizeof(int));
		write(clientfd, &db.rowsize, sizeof(int));
		return send_table(db.columns, db.columnsize, clientfd);
	}

	to_send_columns = malloc((size-2) * sizeof(column_t*));
	if(to_send_columns == NULL){
		return -1;
	}

	for(i = 0; i < size-2; i++){
		columnindex = get_column_index(query[i]);
		if(columnindex == -1){
			free(to_send_columns);
			return -1;
		}
		to_send_columns[i] = db.columns[columnindex];
	}

	sendsize = size - 2;

	write(clientfd, &sendsize, sizeof(int));
	write(clientfd, &db.rowsize, sizeof(int));
	res = send_table(to_send_columns, sendsize, clientfd);
	free(to_send_columns);
	return res;

}


int get_column_index(char *columnname){
	int i=0;
	for(i = 0; i < db.columnsize; i++){
		if(strcmp(columnname, db.columns[i].entries[0]) == 0){
			return i;
		}
	}
	return -1;

}


char** parse_query(char *query, int *size){
	char **substrings;
	char *saveptr = NULL;
	char *str = NULL;
	int q_flag = FALSE;
	int cap = 20;
	*size = 0;

	substrings = malloc(cap * sizeof(char*));
	if(substrings == NULL){
		return NULL;
	}
	str = strtok_r(query, " ,\n=;", &saveptr);
	do{
		
		if(str[0] == '"' || str[0] == '\''){
			q_flag = TRUE;
			substrings[*size] = malloc((strlen(str) + 1) * sizeof(char));
			strcpy(substrings[*size], &str[1]);
			if(str[strlen(str) - 1] == '"' || str[strlen(str) - 1] == '\''){
				q_flag = FALSE;
				substrings[*size][strlen(substrings[*size]) -1] = '\0';
				(*size)++;
			}
		}
		else if(q_flag == TRUE){
			if(str[strlen(str) - 1] == '"' || str[strlen(str) - 1] == '\''){
				q_flag = FALSE;
				substrings[*size] = realloc(substrings[*size], (strlen(substrings[*size]) + strlen(str) + 3) * sizeof(char));
				strcat(substrings[*size], " ");
				strcat(substrings[*size], str);
				substrings[*size][strlen(substrings[*size]) -1] = '\0';
				(*size)++;
			}
			else{
				substrings[*size] = realloc(substrings[*size], (strlen(substrings[*size]) + strlen(str) + 3) * sizeof(char));
				strcat(substrings[*size], " ");
				strcat(substrings[*size], str);
			}
		}
		else{
			substrings[*size] = malloc((strlen(str) + 1) * sizeof(char));
			strcpy(substrings[*size], str);
			(*size)++;
		}

		if((*size) == cap){
			cap *= 2;
			substrings = realloc(substrings, cap * sizeof(char*));
		}
		str = strtok_r(NULL, " ,\n=;", &saveptr);

	}while(str != NULL);

	return substrings;

}


int send_table(column_t *cols, int size, int clientfd){
	int i, j;

	for(i = 0; i < size; i++){
		for(j = 0; j < db.rowsize; j++){
			if(write(clientfd, cols[i].entries[j], ENTRY_SIZE) == -1){
				return -1;
			}
		}
	}
	return db.rowsize;

}


void get_reader_lock(){
	pthread_mutex_lock(&rwmutex);
	while( (AW + WW) > 0){
		WR++;
		pthread_cond_wait(&okToRead, &rwmutex);
		WR--;
	}
	AR++;
	pthread_mutex_unlock(&rwmutex);

}


void release_reader_lock(){
	pthread_mutex_lock(&rwmutex);
	AR--;
	if(AR == 0 && WW > 0){
		pthread_cond_signal(&okToWrite);
	}
	pthread_mutex_unlock(&rwmutex);
	
}


void get_writer_lock(){
	pthread_mutex_lock(&rwmutex);
	while( (AW + AR) > 0){
		WW++;
		pthread_cond_wait(&okToWrite, &rwmutex);
		WW--;
	}
	AW++;
	pthread_mutex_unlock(&rwmutex);

}


void release_writer_lock(){
	pthread_mutex_lock(&rwmutex);
	AW--;
	if(WW > 0){
		pthread_cond_signal(&okToWrite);
	}
	else if(WR > 0){
		pthread_cond_signal(&okToRead);
	}
	pthread_mutex_unlock(&rwmutex);

}


int create_pool_threads(int size){
	int i;
	int error;
	char log[BUFFER_LEN];

	for(i = 0; i < size; i++){
		pool_threads[i].number = i;
		if( (error = pthread_create(&pool_threads[i].tid, NULL, pool_thread_func, &pool_threads[i].number)) ){
			sprintf(log, "Failed to create pool thread: %s\n", strerror(error));
			write_log(log);
			return -1;
		}
		pool_threads[i].thread_created = TRUE;
	}

	return 0;

}


int create_pool_arr(int size){
	char log[BUFFER_LEN];

	pool_threads = malloc(size * sizeof(thread_info));
	if(pool_threads == NULL){
		sprintf(log, "// Allocating array // ERRNO: %d - %s", errno, strerror (errno));
		write_log(log);
		return -1;
	}

	return 0;

}


int parse_command_line_opt(int argc, char **argv, char** logfile, char** datafile, int* port, int* poolsize){
	int ch;
	int opscount = 0;
	while((ch = getopt(argc,argv,"p:o:l:d:")) != -1){
		switch(ch){

			case 'p':
				if(is_str_a_int(optarg) == TRUE){
					(*port) = atoi(optarg);
					opscount++;
				}
				else{
					fprintf(stderr,"Please use integer for PORT.\n");
					return -1;
				}
				break;

			case 'o':
				(*logfile) = optarg;
				opscount++;
				break;

			case 'l':
				if(is_str_a_int(optarg) == TRUE){
					(*poolsize) = atoi(optarg);
					opscount++;
				}
				else{
					fprintf(stderr,"Please use integer for poolsize.\n");
					return -1;
				}
				break;

			case 'd':
				(*datafile) = optarg;
				opscount++;
				break;

			case '?':
				// Default err for getopt() is ok for unusual situations
				return -1;
		}
	}
	if (optind != argc) {
		fprintf(stderr,"Unknown arguments given.\n");
		return -1;
	}
	else if (opscount != 4){
		fprintf(stderr,"Please enter all needed arguments for correct usage.\n");
		return -1;
	}
	else if ((*poolsize) < 2){
		fprintf(stderr,"Please enter a number greater than 1 for poolsize.\n");
		return -1;
	}

	return 0;

}


int is_str_a_int(char *str){
	int i;
	for(i=0; str[i] != '\0'; i++){
		if(isdigit(str[i]) == 0)
			return FALSE;
	}
	return TRUE;

}


void write_log(char *msg){

	pthread_mutex_lock(&pmutex);

	time_t time_;
	struct tm *time_s;

	time_ = time(NULL);
	time_s = localtime(&time_);

	fprintf(logfp, "(%02d:%02d:%02d) - %s\n", time_s->tm_hour, time_s->tm_min, time_s->tm_sec, msg);


	pthread_mutex_unlock(&pmutex);

}


int init_socket_queue(){
	queue = malloc(sizeof(SocketQueue));
	if(queue == NULL){
		return -1;
	}

	queue->head = NULL;
	queue->tail = NULL;

	return 0;

}


int add_socket_to_queue(int socket_){

	QueueNode *newnode = malloc(sizeof(QueueNode));
	if(newnode == NULL){
		return -1;
	}
	newnode->socketfd = socket_;
	newnode->next = NULL;

	if(queue->head == NULL){
		queue->head = newnode;
		queue->tail = newnode;

	}
	else{
		queue->tail->next = newnode;
		queue->tail = newnode;
	}

	return 0;

}


int get_socket_from_queue(int *return_socket){

	QueueNode *toFree = queue->head;
	if(toFree == NULL){
		return -1;
	}

	*return_socket = queue->head->socketfd;
	queue->head = queue->head->next;

	if(queue->head == NULL){
		queue->tail = NULL;
	}

	free(toFree);
	return 0;

}


void free_queue(){
	QueueNode *curr;
	QueueNode *next;

	if(queue == NULL){
		return;
	}

	curr = queue->head;

	while(curr != NULL){

		next = curr->next;
		free(curr);
		curr = next;
	}

	free(queue);

}


int fill_db(FILE *fp){
	char ch;
	int q_flag = FALSE;
	int l_flag = FALSE;
	int t_errno;
	int t_len, t_column, t_row;
	char log[BUFFER_LEN];

	t_errno = errno;
	ch = '0';
	t_len = 0;
	t_column = 0;
	t_row = 0;

	while(ch != EOF){
		
		ch = (char)fgetc(fp);

		if(ch == EOF){
			if(l_flag)
				break;
			else{
				db.columns[t_column].entries[t_row][t_len] = '\0';
				break;
			}
		}

		if(ch == ',' && !q_flag){
			db.columns[t_column].entries[t_row][t_len] = '\0';
			t_len = 0;
			t_column++;
		}
		else if(ch == '"'){
			if(!q_flag){
				q_flag = TRUE;
			}
			else{
				q_flag = FALSE;
			}
		}
		else if(ch == '\n'){
			db.columns[t_column].entries[t_row][t_len] = '\0';
			t_len = 0;
			t_column = 0;
			t_row++;
			l_flag = TRUE;
		}
		else{
			if(t_len != (ENTRY_SIZE -1)){
				db.columns[t_column].entries[t_row][t_len] = ch;
				t_len++;
			}
		}
	}
	if(errno != t_errno){
		sprintf(log, "Failed to read from file: %s\n", strerror(errno));
		write_log(log);
		return -1;
	}

	return 0;

}


int create_db(FILE *fp){
	char ch;
	int q_flag = FALSE;
	int l_flag = FALSE;
	int t_errno;
	int i, j;
	char log[BUFFER_LEN];

	db.columns = NULL;
	db.columnsize = 0;
    db.rowsize = 0;
    t_errno = errno;
	ch = '0';

	while(ch != EOF){
		ch = (char)fgetc(fp);

		if(ch == ',' && !q_flag){
			db.columnsize++;
		}
		else if(ch == '"'){
			if(!q_flag){
				q_flag = TRUE;
			}
			else{
				q_flag = FALSE;
			}
		}
		else if(ch == '\n'){
			db.columnsize++;
			db.rowsize++;
			break;
		}
	}

	while(ch != EOF){
		ch = (char)fgetc(fp);

		if(ch == EOF && !l_flag){
			db.rowsize++;
		}

		l_flag = FALSE;
		if(ch == '\n'){
			db.rowsize++;
			l_flag = TRUE;
		}
	}
	if(errno != t_errno){
		sprintf(log, "Failed to read from file: %s\n", strerror(errno));
		write_log(log);
		return -1;
	}
	rewind(fp);

	db.columns = malloc(db.columnsize * sizeof(column_t));
	if(db.columns == NULL){
		sprintf(log, "Allocation error: %s\n", strerror(errno));
		write_log(log);
		return -1;
	}

	for(i = 0; i < db.columnsize; i++){
		db.columns[i].entries = malloc(db.rowsize * sizeof(char*));
		if(db.columns[i].entries == NULL){
			sprintf(log, "Allocation error: %s\n", strerror(errno));
			write_log(log);
			return -1;
		}

		for(j = 0; j < db.rowsize; j++){
			db.columns[i].entries[j] = malloc(ENTRY_SIZE * sizeof(char));
			if(db.columns[i].entries[j] == NULL){
				sprintf(log, "Allocation error: %s\n", strerror(errno));
				write_log(log);
				return -1;
			}
			memset(db.columns[i].entries[j], 0, ENTRY_SIZE * sizeof(char));
		}
	}
    return 0;

}


void free_db(){
	int i, k;
	if(db.columns == NULL)
		return;

	for(i = 0; i < db.columnsize; i++){

		if(db.columns[i].entries == NULL)
			return;

		for(k = 0; k < db.rowsize; k++){
			if(db.columns[i].entries[k] == NULL)
				return;
			free(db.columns[i].entries[k]);
		}
		free(db.columns[i].entries);
	}
	free(db.columns);

}


void free_string_array(char** arr, int size){
	int i;

	for(i = 0; i < size; i++){
		free(arr[i]);
	}
	free(arr);

}


int init_sigint_handling(sigset_t *oldmask){
	struct sigaction sa;
	sigset_t mask;
	int error;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &int_handler;
	if ((sigaction(SIGINT, &sa, NULL) == -1) ){
		return -1;
	}

	if( (sigemptyset(&mask) == -1) || (sigaddset(&mask, SIGINT) == -1) ){
		return -1;
	}

	if( (error = pthread_sigmask(SIG_BLOCK, &mask, oldmask)) == -1){
		return -1;
	}

	return 0;

}


void join_all_threads(int size){
	int i;
	int error;
	void *res;
	char log[BUFFER_LEN];

	for( i = 0; i < size; i++){
		if(pool_threads[i].thread_created){
			error = pthread_join(pool_threads[i].tid, &res);
			if(error != 0){
				sprintf(log, "Failed to join thread %d", i);
				write_log(log);
			}
		}
	}

}


void clean_up(){
	free_db();
	free_queue();

	if (logfp != NULL) 			
		fclose(logfp);
	if (pool_threads != NULL) 	
		free(pool_threads);
	if (fp != NULL)
		fclose(fp);

	sem_close(temp_sem);
	sem_unlink(EXCL_NAME);

}