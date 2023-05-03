#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <semaphore.h>
#include <signal.h>
#include <ctype.h>

#define TRUE 	1
#define FALSE 	0


#define SHARED_MEM_NAME 	"clinic"
#define CITIZEN_DATA_NAME 	"citizens"




struct args_t{
	int no_nurse;
	int no_vaccinator;
	int no_citizen;
	int buffer_size;
	int t_times;
	char *in_filename;
};
typedef struct args_t args_t;


struct clinic_t{
	int n_done;
	int vacc_left;
	int citizen_left;
	int total_vacc;
	int vacc_1;
	int vacc_2;
	
	sem_t buffer_empty;
	sem_t vacc1_full;
	sem_t vacc2_full;
	sem_t sm_mutex;
	sem_t ct_mutex;
	sem_t available_ct;
	sem_t end_barrier;
};
typedef struct clinic_t clinic_t;

struct citizen_info{
	int pid;
	int busy;
};
typedef struct citizen_info citizen_info;

void err_in_child();
void cleanup_and_terminate();
void term_childeren_and_term();
void citizen(int , args_t *);
void vaccinator(int , args_t *);
int nurse(int , args_t *);
int init_citizen_data(args_t *, citizen_info *);
int init_semaphores(int , int);
int init_shared_memory(args_t *params);
int init_signal_mask();
int parse_command_line_opt(int , char *const *, args_t *);
void print_correct_usage();
int is_str_a_int(char *);
void print_errno(char * str);
void sigusr_handler(int );
void sort_citizens_by_age(int );


clinic_t *clinic_p = NULL;
citizen_info *citizen_sm = NULL;
sigset_t oldmask;
int input_fd = -1;


int int_flag = FALSE;
int sigusr1_flag = FALSE;
int sigusr2_flag = FALSE;


void int_handler(int signo){
	int_flag = TRUE;
}


int main(int argc, char *const *argv){
	args_t params;
	struct sigaction sa;
	int i;
	int pid;
	citizen_info *citizen_arr;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &int_handler;
	if ((sigaction(SIGINT, &sa, NULL) == -1) ){
		print_errno("SIGINT Handler");
		exit(EXIT_FAILURE);
	}


	if(parse_command_line_opt(argc, argv, &params) != 0){
		print_correct_usage();
		exit(EXIT_FAILURE);
	}


	if(init_signal_mask() == -1){
		print_errno("Signal Masking");
		exit(EXIT_FAILURE);
	}


	if(init_shared_memory(&params) == -1){
		print_errno("Shared Memory");
		exit(EXIT_FAILURE);
	}


	if(init_semaphores(params.buffer_size, params.no_citizen) == -1){
		print_errno("Semaphore");
		shm_unlink(SHARED_MEM_NAME);
		exit(EXIT_FAILURE);
	}


	if ((input_fd = open(params.in_filename, O_RDONLY)) == -1){
		print_errno("Input File");
		shm_unlink(SHARED_MEM_NAME);
		exit(EXIT_FAILURE);
	}


	for(i=0; i < params.no_nurse && !int_flag; i++){
		switch(fork()){
			case 0:
				nurse(i+1, &params);
				exit(EXIT_SUCCESS);

			case -1:
				print_errno("Creating Child");
				term_childeren_and_term();
				exit(EXIT_FAILURE);

			default:
				break;

		}
	}

	citizen_arr = (citizen_info *)malloc(sizeof(citizen_info) * params.no_citizen);
	memset(citizen_arr, 0, sizeof(citizen_info) * params.no_citizen);
	init_citizen_data(&params, citizen_arr);
	free(citizen_arr);

	for(i=0; i < params.no_citizen && !int_flag; i++){
		pid = fork();
		switch(pid){
			case 0:
				citizen(i+1, &params);
				exit(EXIT_SUCCESS);

			case -1:
				print_errno("Creating Child");
				term_childeren_and_term();
				exit(EXIT_FAILURE);

			default:
				break;

		}
		citizen_sm[i].pid = pid;
		citizen_sm[i].busy = FALSE;
	}

	sort_citizens_by_age(params.no_citizen);


	for(i=0; i < params.no_vaccinator && !int_flag; i++){
		switch(fork()){
			case 0:
				vaccinator(i+1, &params);
				exit(EXIT_SUCCESS);

			case -1:
				print_errno("Creating Child");
				term_childeren_and_term();
				exit(EXIT_FAILURE);

			default:
				break;

		}
	}

	if(int_flag){
		cleanup_and_terminate();
		fprintf(stderr, "PID: %d // Program interrupted by Interrupt Signal. Sources are cleaned up and exited!\n",getpid());
		exit(EXIT_FAILURE);
	}


	sigsuspend(&oldmask);

	if(int_flag){
		cleanup_and_terminate();
		fprintf(stderr, "PID: %d // Program interrupted by Interrupt Signal. Sources are cleaned up and exited!\n",getpid());
		exit(EXIT_FAILURE);

	}
	else if(sigusr1_flag){
		printf("All citizens have been vaccinated.\n");
		sem_post(&clinic_p->end_barrier);

	}
	else if(sigusr2_flag){
		term_childeren_and_term();
		fprintf(stderr, "PID: %d // An error occured in a child process. Sources are cleaned up and exited!\n",getpid());
		exit(EXIT_FAILURE);

	}
	else{
		term_childeren_and_term();
		fprintf(stderr, "PID: %d // An unknown signal received! Sources are cleaned up and exited!\n",getpid());
		exit(EXIT_FAILURE);

	}

	cleanup_and_terminate();
	printf("The clinicis now closed. Stay healthy.\n");
	return 0;
}

void err_in_child(int _print, char* msg){
	if(_print){
		print_errno(msg);
	}
	else{
		fprintf(stderr, "PID: %d // %s\n",getpid(), msg);
	}
	kill(getppid(), SIGUSR2);
	exit(EXIT_FAILURE);
}


void cleanup_and_terminate(){

	while (wait(NULL) > 0);
	close(input_fd);
	sem_destroy(&clinic_p->buffer_empty);
	sem_destroy(&clinic_p->vacc1_full);
	sem_destroy(&clinic_p->vacc2_full);
	sem_destroy(&clinic_p->sm_mutex);
	sem_destroy(&clinic_p->ct_mutex);
	sem_destroy(&clinic_p->available_ct);
	sem_destroy(&clinic_p->end_barrier);
	munmap(clinic_p, sizeof(clinic_t));
	shm_unlink(SHARED_MEM_NAME);
	shm_unlink(CITIZEN_DATA_NAME);

}


void citizen(int number, args_t *params){
	int done = FALSE;
	int citizen_i = -1;
	int dose_left = params->t_times;
	int curr_dose;
	int i;
	struct sigaction sa;

	// setting handler of signt to default (explained in report)
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	if ((sigaction(SIGINT, &sa, NULL) == -1) ){
		err_in_child(1, "SIGINT to Default");
		exit(EXIT_FAILURE);
	}

	if ( int_flag == TRUE){
		exit(EXIT_FAILURE);
	}


	while(!done){
		sigsuspend(&oldmask);

		sem_wait(&clinic_p->ct_mutex);
		if (citizen_i == -1){

			for(i = 0; i < params->no_citizen; i++){

				if(citizen_sm[i].pid == getpid()){
					citizen_i = i;
				}

			}

		}


		dose_left--;
		curr_dose = params->t_times - dose_left;
		printf("Citizen %d (pid=%d) is vaccinated for the %d. time\n",number, getpid(), curr_dose);

		if(dose_left > 0){
			citizen_sm[citizen_i].busy = FALSE;
			sem_post(&clinic_p->available_ct);
		}
		else{
			done = TRUE;
			sem_wait(&clinic_p->sm_mutex);
			clinic_p->citizen_left--;
			printf("The citizen is leaving. Remaining citizens to vaccinate: %d\n", clinic_p->citizen_left);

			if(clinic_p->citizen_left == 0){
				kill(getppid(), SIGUSR1);
			}
			sem_post(&clinic_p->sm_mutex);
		}

		sem_post(&clinic_p->ct_mutex);
	}
	exit(EXIT_SUCCESS);
}

void vaccinator(int number, args_t *params){
	int finished = FALSE;
	int found;
	int dose_count = 0;
	int i;
	int curr_ct;
	struct sigaction sa;

	// setting handler of signt to default (explained in report)
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	if ((sigaction(SIGINT, &sa, NULL) == -1) ){
		err_in_child(1, "SIGINT to Default");
		exit(EXIT_FAILURE);
	}

	if ( int_flag == TRUE){
		exit(EXIT_FAILURE);
	}

	while(!finished){
		found = FALSE;


		sem_wait(&clinic_p->sm_mutex);

		if(clinic_p->vacc_left > 0){
			clinic_p->vacc_left--;
		}
		else{
			finished = TRUE;
		}
		sem_post(&clinic_p->sm_mutex);


		if (!finished){
			// waiting for availaible citizen to vaccinate
			sem_wait(&clinic_p->available_ct);
			sem_wait(&clinic_p->ct_mutex);
			for(i = 0; i < params->no_citizen && !found; i++){
				if(!citizen_sm[i].busy){
					curr_ct = citizen_sm[i].pid;
					citizen_sm[i].busy = TRUE;
					found = TRUE;
				}
			}
			sem_post(&clinic_p->ct_mutex);

			// wait for vaccines
			sem_wait(&clinic_p->vacc1_full);
			sem_wait(&clinic_p->vacc2_full);
			sem_wait(&clinic_p->sm_mutex);

			clinic_p->vacc_1--;
			clinic_p->vacc_2--;
			dose_count++;

			printf("Vaccinator %d (pid=%d) is inviting citizen pid=%d to the clinic. Clinic has %d vaccine1 and %d vaccine2.\n",number, getpid(), curr_ct, clinic_p->vacc_1, clinic_p->vacc_2);
			kill(curr_ct, SIGUSR1);


			sem_post(&clinic_p->sm_mutex);
			// free 2 spaces in buffer
			sem_post(&clinic_p->buffer_empty);
			sem_post(&clinic_p->buffer_empty);
		}

	}
	sem_wait(&clinic_p->end_barrier);
	sem_post(&clinic_p->end_barrier);
	printf("Vaccinator %d (pid=%d) vaccinated %d doses. ",number, getpid(), dose_count);

}

int nurse(int number, args_t *params){
	char ch = '\0';
	int finished = FALSE;
	int read_c = 0;
	struct sigaction sa;

	// setting handler of signt to default (explained in report)
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	if ((sigaction(SIGINT, &sa, NULL) == -1) ){
		err_in_child(1, "SIGINT to Default");
		exit(EXIT_FAILURE);
	}

	if ( int_flag == TRUE){
		exit(EXIT_FAILURE);
	}

	while (!finished){


		sem_wait(&clinic_p->buffer_empty);
		sem_wait(&clinic_p->sm_mutex);

		if(clinic_p->n_done == TRUE){
			finished = TRUE;
			sem_post(&clinic_p->sm_mutex);
		}
		else{
			read_c = read(input_fd, &ch, 1);
			// Error check
			if(read_c == 1 && (ch == '1' || ch == '2')){
				clinic_p->total_vacc++;
				printf("Nurse %d (pid=%d) has brought vaccine %c: the clinic has %d vaccine1 and %d vaccine2.\n",number, getpid(), ch, clinic_p->vacc_1, clinic_p->vacc_2);
				if( clinic_p->total_vacc == (params->no_citizen * params->t_times * 2)){
					printf("Nurses have carried all vaccines to the buffer, terminating.\n" );
					clinic_p->n_done = TRUE;
					finished = TRUE;
				}

				if(ch == '1'){
					clinic_p->vacc_1++;
					sem_post(&clinic_p->sm_mutex);
					sem_post(&clinic_p->vacc1_full);
				}
				else if(ch == '2'){
					clinic_p->vacc_2++;
					sem_post(&clinic_p->sm_mutex);
					sem_post(&clinic_p->vacc2_full);
				}
			}
			else{
				sem_post(&clinic_p->sm_mutex);
				err_in_child(1, "SIGINT to Default");
			}
		}
	}
	return 0;


}

int init_citizen_data(args_t *params, citizen_info *citizens){
	int sm_fd = -1;

	if ( (sm_fd = shm_open(CITIZEN_DATA_NAME, O_CREAT | O_RDWR , S_IRUSR | S_IWUSR)) == -1){ 
		return -1;
	}

	if( ftruncate(sm_fd, sizeof(citizen_info)*params->no_citizen) == -1){
		close(sm_fd);
		shm_unlink(CITIZEN_DATA_NAME);
		return -1;
	}

	citizen_sm = mmap(NULL, sizeof(citizen_info)*params->no_citizen, PROT_READ | PROT_WRITE, MAP_SHARED, sm_fd, 0);
	close(sm_fd);
	if(citizen_sm == MAP_FAILED){
		shm_unlink(CITIZEN_DATA_NAME);
		return -1;
	}


	memcpy(citizen_sm, citizens, sizeof(citizen_info)*params->no_citizen);

	return 0;

}


int init_semaphores(int buffer_size, int no_citizen){
	if( sem_init(&clinic_p->buffer_empty, 1, buffer_size) == -1){
		return -1;
	}

	if( sem_init(&clinic_p->vacc1_full, 1, 0) == -1){
		return -1;
	}

	if( sem_init(&clinic_p->vacc2_full, 1, 0) == -1){
		return -1;
	}

	if( sem_init(&clinic_p->sm_mutex, 1, 1) == -1){
		return -1;
	}

	if( sem_init(&clinic_p->ct_mutex, 1, 1) == -1){
		return -1;
	}

	if( sem_init(&clinic_p->available_ct, 1, no_citizen) == -1){
		return -1;
	}

	if( sem_init(&clinic_p->end_barrier, 1, 0) == -1){
		return -1;
	}

	return 0;
}


int init_shared_memory(args_t *params){
	clinic_t init_c;
	int sm_fd = -1;

	if ( (sm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR , S_IRUSR | S_IWUSR)) == -1){ 
		return -1;
	}

	if( ftruncate(sm_fd, sizeof(clinic_t)) == -1){
		close(sm_fd);
		shm_unlink(SHARED_MEM_NAME);
		return -1;
	}

	clinic_p = mmap(NULL, sizeof(clinic_t), PROT_READ | PROT_WRITE, MAP_SHARED, sm_fd, 0);
	close(sm_fd);
	if(clinic_p == MAP_FAILED){
		shm_unlink(SHARED_MEM_NAME);
		return -1;
	}

	init_c.n_done = FALSE;
	init_c.citizen_left = params->no_citizen;
	init_c.vacc_left = params->no_citizen * params->t_times;
	init_c.vacc_1 = 0;
	init_c.vacc_2 = 0;
	init_c.total_vacc = 0;
	

	memcpy(clinic_p, &init_c, sizeof(clinic_t));

	return 0;

}

void term_childeren_and_term(){
	killpg(0, SIGINT);
	cleanup_and_terminate();
}

int init_signal_mask(){
	struct sigaction sa;
	sigset_t maskusr;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &sigusr_handler;
	if ((sigaction(SIGUSR1, &sa, NULL) == -1) || (sigaction(SIGUSR2, &sa, NULL) == -1)){
		return -1;
	}

	if( (sigemptyset(&maskusr) == -1) || (sigaddset(&maskusr, SIGUSR1) == -1) || (sigaddset(&maskusr, SIGUSR2) == -1)){
		return -1;
	}

	if( sigprocmask(SIG_BLOCK, &maskusr, &oldmask) == -1){
		return -1;
	}

	return 0;
}

int parse_command_line_opt(int argc, char *const *argv, args_t *params){
	int ch;
	int opscount = 0;
	int tmp;
	while((ch = getopt(argc,argv,"n:v:c:b:t:i:")) != -1){
		switch(ch){

			case 'n':
				if(is_str_a_int(optarg) == TRUE && (tmp = atoi(optarg)) >= 2){
					params->no_nurse = tmp;
					opscount++;
				}
				else 	return -1;
				break;

			case 'v':
				if(is_str_a_int(optarg) == TRUE && (tmp = atoi(optarg)) >= 2){
					params->no_vaccinator = tmp;
					opscount++;
				}
				else 	return -1;
				break;

			case 'c':
				if(is_str_a_int(optarg) == TRUE && (tmp = atoi(optarg)) >= 3){
					params->no_citizen = tmp;
					opscount++;
				}
				else 	return -1;
				break;

			case 'b':
				if(is_str_a_int(optarg) == TRUE && (tmp = atoi(optarg)) >= 2){
					params->buffer_size = tmp;
					opscount++;
				}
				else 	return -1;
				break;

			case 't':
				if(is_str_a_int(optarg) == TRUE && (tmp = atoi(optarg)) >= 1){
					params->t_times = tmp;
					opscount++;
				}
				else 	return -1;
				break;

			case 'i':
				params->in_filename = optarg;
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
	else if (opscount != 6){
		return -1;
	}
	else if (params->buffer_size < (params->t_times * params->no_citizen + 1)){
		return -1;
	}

	return 0;
}

void print_correct_usage(){
	fprintf(stderr,"Correct usage:\n\tn >= 2: the number of nurses (integer)\n\tv >= 2: the number of vaccinators (integer)\n\t");
	fprintf(stderr,"b >= tc+1: size of the buffer (integer)\n\tt >= 1: how many times each citizen must receive the 2 shots (integer)\n\ti: pathname of the input file\n");
}

int is_str_a_int(char *str){
	int i;
	for(i=0; str[i] != '\0'; i++){
		if(isdigit(str[i]) == 0)
			return FALSE;
	}
	return TRUE;
}

void print_errno(char * str){
	fprintf(stderr, "PID: %d // ERRNO: %d // %s // - %s \n",getpid(), errno, str, strerror (errno));
}

void sigusr_handler(int signo){
	
	if(signo == SIGUSR1){
		sigusr1_flag = TRUE;
	}
	else if (signo == SIGUSR2){
		sigusr2_flag = TRUE;
	}
}

void sort_citizens_by_age(int size){
	int i, k, min;
	int _pid;


	for(i = 0; i < size -1; i++){
		min = i;

		for(k = i + 1; k < size; k++){
			if( citizen_sm[k].pid < citizen_sm[min].pid){
				min = k;
			}
		}

		_pid = citizen_sm[min].pid;
		citizen_sm[min].pid = citizen_sm[i].pid;
		citizen_sm[i].pid = _pid;
	}
}
