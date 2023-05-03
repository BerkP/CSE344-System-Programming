#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <signal.h>
#include <ctype.h>
#include <pthread.h>
#include <fcntl.h>


#define TRUE 	1
#define FALSE 	0

#define STDNT_NAME_LEN 50
#define LINE_MAX_LEN 100


struct student_t{
	char name[STDNT_NAME_LEN];
	int quality;
	int speed;
	int price;

	int solved_hw;
	int earning;

	pthread_t tid;
	int student_no;
	int thread_created;

	int available;
	char curr_hw;
	sem_t start_work;
};
typedef struct student_t student_t;


struct HW_Queue{
	struct HWQ_Node *head;
	struct HWQ_Node *tail;
};
typedef struct HW_Queue HW_Queue;


struct HWQ_Node{
	char prio;
	struct HWQ_Node *next;

};
typedef struct HWQ_Node HWQ_Node;


int money;
int success = FALSE;
int out_of_money = FALSE;
student_t *students = NULL;
HW_Queue *queue = NULL;
int terminate = FALSE;
int user_int = FALSE;


sem_t queueempty;
sem_t available_stdnt;
sem_t hw_mx;
sem_t stdnt_mx;
sem_t h_end_barrier;


void *student_for_hire(void *);
void *thread_h(void *);
int make_prio_comparison(int , int , char );
int get_suitable_student(int , char );
int create_student_threads(int );
int init_semaphores(int );
int create_students_arr(char* );
int fill_students_arr(char* );
int check_command_line_args(int , char **);
int is_str_a_int(char *);
int init_hw_queue();
int add_hw_to_queue(char );
int get_hw_from_queue(char *);
void free_students(int size);
void free_queue();
void free_semaphores();
void notify_all_students(int );
void join_all_students(int );
void print_created_students(int );
void print_student_results(int );
void successfully_exit(int );
void cancel_all_students(int );
void terminate_program(int );
void check_for_error_and_interrupt(int );
void int_handler(int );
int init_sigint_handling(sigset_t *);



int main(int argc, char  **argv){
	int size;
	int error;
	char curr_hw;
	int student_i;
	pthread_t tid_h;
	sigset_t oldmask;
	student_t *curr_student;
	
	
	if(check_command_line_args(argc, argv) == -1 ){
		fprintf(stderr,"Correct usage:\t./program homeworkFilePath studentsFilePath money(positive integer)\n");
		exit(EXIT_FAILURE);
	}

	if(init_sigint_handling(&oldmask) == -1){
		exit(EXIT_FAILURE);
	}

	if( (size = create_students_arr(argv[2])) == -1){
		free_students(size);
		exit(EXIT_FAILURE);
	}

	if( fill_students_arr(argv[2]) == -1){
		free_students(size);
		exit(EXIT_FAILURE);
	}

	print_created_students(size);	


	if( init_semaphores(size) == -1){
		fprintf(stderr, "Main thread // Init semaphores // ERRNO: %d - %s \n", errno, strerror (errno));
		free_students(size);
		free_semaphores();
		exit(EXIT_FAILURE);
	}

	if (create_student_threads(size) == -1){
		sem_post(&h_end_barrier);
		terminate_program(size);
	}


	if( (error = pthread_create(&tid_h, NULL, thread_h, argv[1]))){
		fprintf(stderr,"Failed to create thread: %s\n", strerror(error));
		sem_post(&h_end_barrier);
		terminate_program(size);
	}

	if( (error = pthread_detach(tid_h)) ){
		fprintf(stderr,"Failed to detach thread: %s\n", strerror(error));
		terminate_program(size);
	}

	if( (error = pthread_sigmask(SIG_SETMASK, &oldmask, NULL)) ){
		fprintf(stderr,"Failed to restore sigint mask: %s\n", strerror(error));
		terminate_program(size);
	}


	while(!terminate){
		check_for_error_and_interrupt(size);
		sem_wait(&queueempty);
		check_for_error_and_interrupt(size);

		sem_wait(&hw_mx);
		check_for_error_and_interrupt(size);


		if (get_hw_from_queue(&curr_hw) == -1){
			terminate = TRUE;
			success = TRUE;
			sem_post(&hw_mx);
			break;
		}
		sem_post(&hw_mx);

		sem_wait(&available_stdnt);
		check_for_error_and_interrupt(size);

		sem_wait(&stdnt_mx);
		student_i = get_suitable_student(size, curr_hw);
		sem_post(&stdnt_mx);
		curr_student = &students[student_i];

		sem_wait(&hw_mx);
		check_for_error_and_interrupt(size);


		if ( (money - curr_student->price) < 0 ){
			out_of_money = TRUE;
			terminate = TRUE;
			success = TRUE;
		}
		else{
			sem_post(&stdnt_mx);
			money -= curr_student->price;

			curr_student->available = FALSE;
			curr_student->curr_hw = curr_hw;
			curr_student->solved_hw += 1;
			curr_student->earning += curr_student->price;
			sem_post(&curr_student->start_work);
			sem_post(&stdnt_mx);
		}

		sem_post(&hw_mx);
		
	}
	
	if(success){
		successfully_exit(size);
	}
	else{
		terminate_program(size);
	}
	
	return 0;

}


void *student_for_hire(void *arg){
	int student_no;
	student_t *info;
	int finished;

	student_no = *((int *)(arg));
	info = &students[student_no];
	finished = FALSE;


	printf("%s is waiting for a homework\n", info->name);

	while(!finished){
		
		sem_wait(&info->start_work);

		if(info->available == TRUE){
			finished = TRUE;
		}
		else{

			sem_wait(&hw_mx);
			printf("%s is solving homework %c for %d, H has %d left.\n", info->name, info->curr_hw, info->price, money);
			sem_post(&hw_mx);

			sleep(6 - info->speed);

			sem_wait(&stdnt_mx);
			printf("%s is waiting for a homework\n", info->name);
			info->available = TRUE;
			sem_post(&stdnt_mx);

			sem_post(&available_stdnt);
		}

	}
	
	return NULL;

}


void *thread_h(void *arg){
	char *filename;
	char ch = '\0';
	int finished = FALSE;
	int read_c = 0;
	int input_fd;

	filename = (char *)(arg);

	if ((input_fd = open(filename, O_RDONLY)) == -1){
		terminate = TRUE;
		fprintf(stderr, "Thread H // Open File // ERRNO: %d - %s \n", errno, strerror (errno));
	}

	if(init_hw_queue() == -1){
		terminate = TRUE;
	}

	while (!finished && !terminate){

		read_c = read(input_fd, &ch, 1);
		//Error check
		if(read_c == -1){
			terminate = TRUE;
			fprintf(stderr, "Thread H // Read File // ERRNO: %d - %s \n", errno, strerror (errno));
		}
		else if(read_c == 0 || ch == '\n'){
			finished = TRUE;
			printf("H has no other homeworks, terminating. \n");
		}
		else if( ch != 'C' && ch != 'S' && ch != 'Q'){
			terminate = TRUE;
			fprintf(stderr, "Thread H // Invalid homework priority\n");
		}
		else{
			sem_wait(&hw_mx);

			if(money == 0){
				terminate = TRUE;
				printf("H has no more money for homeworks, terminating. \n");
			}
			else{
				if ( add_hw_to_queue(ch) == -1){
					terminate = TRUE;	
				}
				else{
					printf("H has a new homework %c; remaining money is %d\n", ch, money);
					sem_post(&queueempty);
				}
			}

			sem_post(&hw_mx);
		}
		
	}

	sem_post(&queueempty);
	sem_post(&h_end_barrier);

	return NULL;

}


int make_prio_comparison(int stdnt_1, int stdnt_2, char prio){
	int res = stdnt_1;

	if(prio == 'Q' && students[stdnt_2].quality > students[stdnt_1].quality){
		res = stdnt_2;
	}
	else if(prio == 'S' && students[stdnt_2].speed > students[stdnt_1].speed){
		res = stdnt_2;
	}
	else if(prio == 'C' && students[stdnt_2].price < students[stdnt_1].price){
		res = stdnt_2;
	}

	return res;

}


int get_suitable_student(int size, char prio){
	int i;
	int return_i = -1;


	for(i = 0; i < size; i++){
		if(return_i == -1 && students[i].available == TRUE){
			return_i = i;
		}
		else if(students[i].available == TRUE){
			return_i = make_prio_comparison(return_i, i, prio);
		}
	}

	return return_i;

}


int create_student_threads(int size){
	int i;
	int error;

	for(i = 0; i < size; i++){
		if( (error = pthread_create(&students[i].tid, NULL, student_for_hire, &students[i].student_no)) ){
			fprintf(stderr,"Failed to create student thread: %s\n", strerror(error));
			return -1;
		}
		students[i].thread_created = TRUE;
	}

	return 0;

}


int init_semaphores(int size){
	if( sem_init(&queueempty, 0, 0) == -1){
		return -1;
	}

	if( sem_init(&available_stdnt, 0, size) == -1){
		return -1;
	}

	if( sem_init(&hw_mx, 0, 1) == -1){
		return -1;
	}

	if( sem_init(&stdnt_mx, 0, 1) == -1){
		return -1;
	}

	if( sem_init(&h_end_barrier, 0, 0) == -1){
		return -1;
	}

	return 0;

}


int create_students_arr(char* filename){
	FILE *fp;
	int t_errno;
	char line[LINE_MAX_LEN];
	char *buffp;
	int no_students = 0;

	if((fp = fopen(filename, "r")) == NULL){
		fprintf(stderr, "// Student File // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	t_errno = errno;
	while((buffp = fgets(line, LINE_MAX_LEN, fp)) != NULL){
		no_students++;
	}
	fclose(fp);

	if(errno != t_errno){
		fprintf(stderr, "// Student File // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	students = malloc(no_students * sizeof(student_t));
	if(students == NULL){
		fprintf(stderr, "// Allocating array // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	return no_students;

}


int fill_students_arr(char* filename){
	FILE *fps;
	int i = 0;
	int t_errno;
	char *buffp;
	int param_scanned;
	char line[LINE_MAX_LEN];


	if((fps = fopen(filename, "r")) == NULL){
		fprintf(stderr, "// Student File // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}


	t_errno = errno;
	while((buffp = fgets(line, LINE_MAX_LEN, fps)) != NULL){

		param_scanned = sscanf(line, "%s %d %d %d", students[i].name, &students[i].quality, &students[i].speed, &students[i].price);
		if(param_scanned != 4){
			fclose(fps);
			fprintf(stderr, "// Student File // sscanf error while reading parameters from line!\n");
			return -1;
		}

		students[i].available = TRUE;
		students[i].curr_hw = '\0';
		students[i].thread_created = FALSE;
		students[i].student_no = i;
		students[i].solved_hw = 0;
		students[i].earning = 0;

		if( sem_init(&students[i].start_work, 0, 0) == -1){
			fclose(fps);
			fprintf(stderr, "// sem_init // ERRNO: %d - %s \n", errno, strerror (errno));
			return -1;
		}

		i++;
	}
	fclose(fps);

	if(errno != t_errno){
		fprintf(stderr, "// Student File // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	return 0;

}


int check_command_line_args(int argc, char **argv){

	if(argc != 4 || !is_str_a_int(argv[3])){
		return -1;
	}

	money = atoi(argv[3]);
	if(money < 1){
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


int init_hw_queue(){
	queue = malloc(sizeof(HW_Queue));
	if(queue == NULL){
		fprintf(stderr, "// Allocating queue // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	queue->head = NULL;
	queue->tail = NULL;

	return 0;

}


int add_hw_to_queue(char prio_){

	HWQ_Node *newnode = malloc(sizeof(HWQ_Node));
	if(newnode == NULL){
		fprintf(stderr, "// Allocating node // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}
	newnode->prio = prio_;
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


int get_hw_from_queue(char *return_hw){

	HWQ_Node *toFree = queue->head;
	if(toFree == NULL){
		return -1;
	}

	*return_hw = queue->head->prio;
	queue->head = queue->head->next;

	if(queue->head == NULL){
		queue->tail = NULL;
	}

	free(toFree);
	return 0;

}


void free_students(int size){
	int i;

	for( i = 0; i < size; i++){
		sem_destroy(&students[i].start_work);
	}

	free(students);

}


void free_queue(){
	HWQ_Node *curr;
	HWQ_Node *next;

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


void free_semaphores(){

	if( sem_destroy(&queueempty) == -1 ){
		return;
	}

	if( sem_destroy(&available_stdnt) == -1 ){
		return;
	}

	if( sem_destroy(&hw_mx) == -1 ){
		return;
	}

	if( sem_destroy(&stdnt_mx) == -1 ){
		return;
	}

	if( sem_destroy(&h_end_barrier) == -1 ){
		return;
	}

}


void notify_all_students(int size){
	int i;

	for( i = 0; i < size; i++){
		sem_post(&students[i].start_work);
	}

}


void join_all_students(int size){
	int i;
	int error;
	void *res;

	for( i = 0; i < size; i++){
		if(students[i].thread_created){
			error = pthread_join(students[i].tid, &res);
			if(error != 0){
				fprintf(stderr,"Failed to join thread: %s\n", strerror(error));
			}
		}
	}

}


void print_created_students(int size){
	int i;

	printf("%d students-for-hire threads have been created. \n", size);
	printf("Name Q S C\n");

	for( i = 0; i < size; i++){
		printf("%s %d %d %d\n", students[i].name, students[i].quality, students[i].speed, students[i].price);
	}

}


void print_student_results(int size){
	int i;

	printf("Homeworks solved and money made by the students:\n");

	for( i = 0; i < size; i++){
		printf("%s %d %d \n", students[i].name, students[i].solved_hw, students[i].earning);
	}

	printf("Money left at G’s account: %dTL \n", money);

}


void successfully_exit(int size){
	//being sure that thread h is ended and detached
	sem_wait(&h_end_barrier);

	notify_all_students(size);
	join_all_students(size);

	if(out_of_money){
		printf("Money is over, closing. \n");
	}
	else{
		printf("No more homeworks left or coming in, closing. \n");
	}

	print_student_results(size);


	free_queue();
	free_students(size);
	free_semaphores();
	exit(EXIT_SUCCESS);

}


void cancel_all_students(int size){

	int i;
	int error;

	for( i = 0; i < size; i++){
		if(students[i].thread_created){
			error = pthread_cancel(students[i].tid);
			if(error != 0){
				fprintf(stderr,"Failed to cancel thread: %s\n", strerror(error));
			}
		}
	}

}


void terminate_program(int size){
	//being sure that thread h is ended and detached
	sem_wait(&h_end_barrier);
	sem_post(&hw_mx);

	cancel_all_students(size);
	join_all_students(size);

	if(user_int){
		printf("Termination signal received, closing.\n");
	}

	free_queue();
	free_students(size);
	free_semaphores();
	exit(EXIT_FAILURE);

}


void check_for_error_and_interrupt(int size){

	if(!terminate){
		return;
	}
	else{
		terminate_program(size);
	}

}


void int_handler(int signo){
	terminate = TRUE;
	user_int = TRUE;

}


int init_sigint_handling(sigset_t *oldmask){
	struct sigaction sa;
	sigset_t mask;
	int error;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &int_handler;
	if ((sigaction(SIGINT, &sa, NULL) == -1) ){
		fprintf(stderr, "Main thread // Sigaction // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	if( (sigemptyset(&mask) == -1) || (sigaddset(&mask, SIGINT) == -1) ){
		fprintf(stderr, "Main thread // Sigset // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	if( (error = pthread_sigmask(SIG_BLOCK, &mask, oldmask)) == -1){
		fprintf(stderr, "Main thread // Sigprocmask // ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	return 0;

}
