#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

#define MAX_N 				50
#define MAX_FIFO_PATH_LEN 	100

#define TRUE 	1
#define FALSE 	0
#define EMPTY 	-1

#define FINISHED 	-1
#define INTRPTD 	-2


struct shared_data{
	int potato_ids[MAX_N];
	int potato_switches[MAX_N];
	int curr_switches[MAX_N];
	int total_hands;
	int curr_hands;
	int curr_potatoes;

	int game_over;
	sem_t barrier;
};
typedef struct shared_data shared_data;


struct args_t{
	int switch_count;
	char *shared_name;
	char *fifofile_name;
	char *named_sem;
};
typedef struct args_t args_t;


int attach_to_shared_memory_and_create_fifo(char [][MAX_FIFO_PATH_LEN], args_t* );
void play_the_game(char [][MAX_FIFO_PATH_LEN], char *);
int read_fifonames(char* , char [][MAX_FIFO_PATH_LEN]);
int send_potato_to_other_hand(int , char* );
int find_potato_index(int );
int send_msg_to_other_hands_to_term(int , char [][MAX_FIFO_PATH_LEN]);
int term_all_hands_and_exit(int , char [][MAX_FIFO_PATH_LEN], char*);
void set_game_over_true();
void check_game_over();
void check_int_flag();
void free_game_resources();
void err_in_attach(int );
int parse_command_line_opt(int , char *const *, args_t *);
int is_str_a_int(char *);
void int_handler(int );

// Process values
sem_t *sm_sem;
shared_data *sdp = NULL;
int process_i;
int totalfifo ;
int int_flag = FALSE;

// File Descriptors for FIFO
int fifo_fd = -1;
int dummy_wr = -1;
int dummy_rd = 1;

// Game values
int has_potato = FALSE;
int curr_potato_id = EMPTY;
int curr_potato_index;


int main(int argc, char *const argv[]){

	char fifonames[MAX_N][MAX_FIFO_PATH_LEN];
	args_t params;
	struct sigaction sa;

	srand(time(NULL));


	// Interrupt handler
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &int_handler;
	if ((sigaction(SIGINT, &sa, NULL) == -1) ){
		fprintf(stderr, "PID: %d //Failed to set sigaction \n", getpid());
		exit(EXIT_FAILURE);
	}

	if(parse_command_line_opt(argc, argv, &params) == -1){
		exit(EXIT_FAILURE);
	}
	

	totalfifo = read_fifonames(params.fifofile_name, fifonames);

	if( (sm_sem = sem_open(params.named_sem, O_CREAT, 0660, 1)) == SEM_FAILED){
		fprintf(stderr, "PID: %d // ERRNO: %d - %s \n", getpid(), errno, strerror (errno));
		exit(EXIT_FAILURE);
	}
	

	if (sem_wait(sm_sem) == -1 && errno == EINTR){
		fprintf(stderr, "PID: %d // Process interrupted by user! Exited! \n", getpid());
		sem_close(sm_sem);
		exit(EXIT_FAILURE);
	}

	attach_to_shared_memory_and_create_fifo(fifonames, &params);

	sem_post(sm_sem);

	printf("PID: %d // Waiting for other processes to start game\n", getpid());

	//interrupt check before the sync barrier
	check_int_flag();


	sem_wait(&sdp->barrier); 
	// interrupt check while sync barrier
	check_int_flag();
	sem_post(&sdp->barrier);

	printf("PID: %d // All hands are initiliazed\n", getpid());
	// check for another process interrupted before game start
	check_game_over(); 
	play_the_game(fifonames, params.shared_name);

	// Program must not reach here.
	fprintf(stderr, "PID: %d // Unexpected error occured! Exited! \n", getpid());
	return 1;
}

int attach_to_shared_memory_and_create_fifo(char fifonames[MAX_N][MAX_FIFO_PATH_LEN], args_t* params){

	int sm_fd = -1;
	struct stat sm_stat;
	shared_data sd;

	sm_fd = shm_open(params->shared_name, O_CREAT | O_RDWR , S_IRUSR | S_IWUSR);
	if (sm_fd == -1){ err_in_attach(TRUE); }


	if(fstat(sm_fd, &sm_stat) == -1){
		close(sm_fd);
		err_in_attach(TRUE);
	}

	if(sm_stat.st_size == 0){
		// Initiliazing the shared structure
		sd.total_hands = totalfifo;
		sd.curr_hands = 0;
		sd.curr_potatoes = 0;
		sd.game_over = FALSE;
		memset(sd.potato_ids, 0, sizeof(sd.potato_ids));
		memset(sd.potato_switches, 0, sizeof(sd.potato_switches));
		memset(sd.curr_switches, 0, sizeof(sd.curr_switches));

		if( ftruncate(sm_fd, sizeof(shared_data)) == -1){
			close(sm_fd);
			err_in_attach(TRUE);
		}

		sdp = mmap(NULL, sizeof(shared_data), PROT_READ | PROT_WRITE, MAP_SHARED, sm_fd, 0);
		close(sm_fd);
		if(sdp == MAP_FAILED){ err_in_attach(TRUE); }

		memcpy(sdp, &sd, sizeof(shared_data));

		if( sem_init(&sdp->barrier, 1, 0) == -1){
			shm_unlink(params->shared_name);
			err_in_attach(TRUE);
		}
	}
	else if (sm_stat.st_size == sizeof(shared_data)){
		//attaching itselft to already initiliazed shared memory
		sdp = mmap(NULL, sizeof(shared_data), PROT_READ | PROT_WRITE, MAP_SHARED, sm_fd, 0);
		close(sm_fd);
		if(sdp == MAP_FAILED){ err_in_attach(TRUE); }
	}
	else{
		fprintf(stderr, "This shared memory has different object. Please unload or enter different memory name.\n");
		close(sm_fd);
		err_in_attach(FALSE);
	}

	if( sdp->curr_hands == sdp->total_hands){
		fprintf(stderr, "No fifos left for this game!\n");
		err_in_attach(FALSE);
	}

	process_i = sdp->curr_hands;

	// Creating fifo file.
	if(mkfifo(fifonames[process_i], S_IWUSR | S_IRUSR) == -1){
		err_in_attach(TRUE);
	}
	// Creating dummy read end with nonblock (for sync)
	if ( (dummy_rd = open(fifonames[process_i], O_RDONLY | O_NONBLOCK)) == -1 ){
		err_in_attach(TRUE);
	}
	// Creating dummy write end for sync)
	if ( (dummy_wr = open(fifonames[process_i], O_WRONLY)) == -1 ){
		err_in_attach(TRUE);
	}
	// Creating actual read end
	if ( (fifo_fd = open(fifonames[process_i], O_RDONLY)) == -1 ){
		err_in_attach(TRUE);
	}

	sdp->curr_hands++;

	if (params->switch_count > 0){
		sdp->potato_ids[process_i] = getpid();
		curr_potato_id = getpid();
		sdp->potato_switches[process_i] = params->switch_count;
		sdp->curr_potatoes++;
		has_potato = TRUE;
	}
	else{
		sdp->potato_ids[process_i] = EMPTY;
	}


	// Unblocking sync barrier.
	if( sdp->curr_hands == sdp->total_hands){
		sem_post(&sdp->barrier);
	}


	return 0;
}

void play_the_game(char fifonames[MAX_N][MAX_FIFO_PATH_LEN], char *shared_name){
	int rndm;
	int message;
	int curr_potato_sw = 0;
	int game_finished = FALSE;

	while(!game_finished){
		//interrupt check at begining of the loop
		if(int_flag){
			fprintf(stderr, "PID: %d //Game interrupted by user! Exiting.\n", getpid());
			term_all_hands_and_exit(INTRPTD,fifonames, shared_name);
		}

		if(!has_potato){

			if(read(fifo_fd, &message, sizeof(int)) != sizeof(int)){
				fprintf(stderr, "PID: %d //READ// ERRNO: %d - %s \n", getpid(), errno, strerror (errno));
				term_all_hands_and_exit(INTRPTD,fifonames, shared_name);
			}

			if(message == FINISHED || message == INTRPTD){
				term_all_hands_and_exit(message,fifonames, shared_name);
			}

			curr_potato_index = find_potato_index(message);
			if(int_flag){
				// interrupt check
				fprintf(stderr, "PID: %d //Game interrupted by user! Exiting.\n", getpid());
				term_all_hands_and_exit(INTRPTD,fifonames, shared_name);
			}
			if(curr_potato_index == -1){
				fprintf(stderr, "PID: %d // Invalid potato id recived! Exiting.\n", getpid());
				term_all_hands_and_exit(INTRPTD,fifonames, shared_name);
			}

			//wait semaphore and check for interrupt
			if (sem_wait(sm_sem) == -1 && errno == EINTR){
				fprintf(stderr, "PID: %d // Process interrupted by user! Exited! \n", getpid());
				term_all_hands_and_exit(INTRPTD,fifonames, shared_name);
			}

			printf("pid=%d receiving potato number %d from %s\n", getpid(), message, fifonames[process_i]);
			sdp->potato_switches[curr_potato_index]--;

			if(sdp->potato_switches[curr_potato_index] == 0){
				sdp->potato_switches[curr_potato_index] = EMPTY;
				sdp->curr_potatoes--;
				printf("pid=%d; potato number %d has cooled down\n", getpid(), message);
			}
			else{
				has_potato = TRUE;
				curr_potato_id = message;
				sdp->curr_switches[curr_potato_index]++;
				curr_potato_sw = sdp->curr_switches[curr_potato_index];
			}

			if(sdp->curr_potatoes == 0){
				sem_post(sm_sem);
				term_all_hands_and_exit(FINISHED,fifonames, shared_name);
			}
			sem_post(sm_sem);
		}
		if(has_potato){
			do{ 
				rndm = rand() % totalfifo; 
			}while(rndm == process_i); 

			if( send_potato_to_other_hand(curr_potato_id, fifonames[rndm]) != -1){
				printf("pid=%d sending potato number %d to %s; this is switch number %d\n", getpid(), curr_potato_id, fifonames[rndm], curr_potato_sw + 1);
			}
			else{
				fprintf(stderr, "PID: %d // SENDING //ERRNO: %d - %s \n", getpid(), errno, strerror (errno));
				term_all_hands_and_exit(INTRPTD,fifonames, shared_name);
			}
			has_potato = FALSE;
			curr_potato_id = EMPTY;
		}
	}
}

int term_all_hands_and_exit(int term_no, char fifonames[MAX_N][MAX_FIFO_PATH_LEN], char* shared_name){
	sem_wait(sm_sem);
	//no need to check for interrupt
	//because it is a exit function

	if(sdp->game_over != TRUE){
		sdp->game_over = TRUE;
		send_msg_to_other_hands_to_term(term_no,fifonames);
		shm_unlink(shared_name);

		if(term_no == FINISHED){
			printf("GAME FINISHED!\n");
		}
	}
	else{
		if(term_no == FINISHED){
			printf("GAME FINISHED!\n");
		}
		else{
			printf("Game is terminated by another hand(process).\n");
		}
	}
	sem_post(sm_sem);
	free_game_resources();

	if(term_no == FINISHED){
		exit(EXIT_SUCCESS);
	}
	else{
		exit(EXIT_FAILURE);
	}
}

int send_msg_to_other_hands_to_term(int term_no, char fifonames[MAX_N][MAX_FIFO_PATH_LEN]){
	int i;
	int rec_fd;

	for(i = 0; i<totalfifo; i++){
		if(i != process_i){
			if ((rec_fd = open(fifonames[i], O_WRONLY)) == -1 ){
				break;
			}
			if(write(rec_fd, &term_no, sizeof(int)) != sizeof(int)){
				break;
			}
			close(rec_fd);
		}
	}
	return 0;
}

int send_potato_to_other_hand(int potato_id, char* fifoname){
	int rec_fd;
	if ( (rec_fd = open(fifoname, O_WRONLY | O_NONBLOCK)) == -1 ){
		return -1;
	}
	if(write(rec_fd, &potato_id, sizeof(int)) != sizeof(int)){
		return -1;
	}
	if(close(rec_fd) == -1){
		return -1;
	}
	return 0;
}

int find_potato_index(int potato_id){
	int i;
	sem_wait(sm_sem);
	for(i=0; i < sdp->total_hands; i++){
		if(sdp->potato_ids[i] == potato_id && sdp->potato_switches[i] != EMPTY){
			sem_post(sm_sem);
			return i;
		}
	}
	sem_post(sm_sem);
	return -1;
}

void set_game_over_true(){
	//no need to check for error because its pre-exit function
	sem_wait(sm_sem);
	sdp->game_over = TRUE;
	sem_post(sm_sem);
}

void check_game_over(){
	sem_wait(sm_sem);
	if(sdp->game_over){
		sem_post(sm_sem);
		free_game_resources();
		fprintf(stderr, "PID: %d // Another hand(process) interrupted before game start! Exited! \n", getpid());
		exit(EXIT_FAILURE);
	}
	sem_post(sm_sem);
}

void check_int_flag(){
	if(int_flag){
		set_game_over_true();
		free_game_resources();
		fprintf(stderr, "PID: %d // Process interrupted by user! Exited! \n", getpid());
		exit(EXIT_FAILURE);
	}
}

void free_game_resources(){
	sem_close(sm_sem);
	close(dummy_rd);
	close(dummy_wr);
	close(fifo_fd);
}

int read_fifonames(char* filename, char fifonames[MAX_N][MAX_FIFO_PATH_LEN]){
	FILE *fp;
	int totalfifo = 0;
	char* buffp;
	int t_errno;
	int len;

	if((fp = fopen(filename,"r")) == NULL){
		fprintf(stderr, "Failed to open the file\n");
		exit(EXIT_FAILURE);
	}

	t_errno = errno;
	while((buffp = fgets(fifonames[totalfifo], MAX_FIFO_PATH_LEN, fp)) != NULL){
		len = strlen(fifonames[totalfifo]);
		if(fifonames[totalfifo][len-1] == '\n'){
			fifonames[totalfifo][len-1] = '\0';
		}
		totalfifo++;
		if(totalfifo == MAX_N){
			fclose(fp);
			fprintf(stderr, "There are more fifo names than MAX_N.\n");
			exit(EXIT_FAILURE);
		}
	}
	fclose(fp);
	if(errno != t_errno){
		fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
		exit(EXIT_FAILURE);
	}
	return totalfifo;
}

void err_in_attach(int print_errno){
	if(print_errno){
		fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
	}
	// Giving lock back and closing sem
	sem_post(sm_sem);
	sem_close(sm_sem);

	// Ignoring error chance of close() because already there is an error and this is exit function.
	if(dummy_rd != -1){
		close(dummy_rd);
	}
	if(dummy_wr != -1){
		close(dummy_wr);
	}
	if(fifo_fd != -1){
		close(fifo_fd);
	}
	exit(EXIT_FAILURE);
}

int parse_command_line_opt(int argc, char *const *argv, args_t *params){
	int ch;
	int opscount = 0;
	while((ch = getopt(argc,argv,"b:s:f:m:")) != -1){
		switch(ch){

			case 's':
				params->shared_name = optarg;
				opscount++;
				break;

			case 'f':
				params->fifofile_name = optarg;
				opscount++;
				break;

			case 'b':
				if(is_str_a_int(optarg) == TRUE){
					params->switch_count = atoi(optarg);
					opscount++;
				}
				else{
					fprintf(stderr,"Please use digits for haspotatoornot.\n");
					return -1;
				}
				break;

			case 'm':
				params->named_sem = optarg;
				opscount++;
				break;

			case '?':
				// Default err for getopt() is ok for unusual situations
				return -1;
		}
	}
	if (optind != argc) {
		fprintf(stderr,"Unknown arguments given. Please read manual for correct usage.\n");
		return -1;
	}
	else if (opscount != 4){
		fprintf(stderr,"Please enter all needed arguments for correct usage.\n");
		return -1;
	}
	else if (params->switch_count < 0){
		fprintf(stderr,"Please enter 0 or positive integer for haspotatoornot.\n");
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

void int_handler(int dummy){
	int_flag = TRUE;
}