#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>

#define TRUE 	1
#define FALSE 	0
#define MAX_ROWLENGTH 300
#define TOTAL_ROW 8


int err_occured = FALSE;
int err_flag = FALSE;
int is_child = FALSE;

sig_atomic_t child_exit_status;
FILE* fp;


int child_p(int , sigset_t *);
void read_lines_to_buffers(char [TOTAL_ROW][MAX_ROWLENGTH], struct flock *, sigset_t *);
void read_coordinates_from_line(char* , float [], float [], struct flock *, sigset_t *);
void write_result_to_end_of_line(char [], float , struct flock *, sigset_t *);
int lagrange_interpolating(float *, int , float , float [], float []);
int get_coffs(int , float [], float [], float []);
int calculate_error_from_line(float* , char [MAX_ROWLENGTH], int );
void error_in_child(sigset_t *, char* ,  int );
void send_siguser2_to_exit(pid_t [], sigset_t *);
void int_handler(int );

void handler(int signo){
	int status;

	if(signo == SIGUSR2){
		err_flag = TRUE;
		err_occured = TRUE;
	}
	else if(signo == SIGCHLD){

		wait(&status);
		printf("exited\n");
		child_exit_status = status;
	}

}


int main(int argc, char const *argv[])
{
	struct sigaction sa;
	struct sigaction sa_2;
	sigset_t maskusr, oldmask;
	pid_t c[8];
	int i;
	char line_buffer[MAX_ROWLENGTH] = "";
	char* buffp;
	float sum, avg;

	if(argc != 2){
		fprintf(stderr, "Wrong usage! For Correct Usage: ./program filename\n");
		exit(1);
	}

	if( (fp = fopen(argv[1],"r+")) == NULL){
		fprintf(stderr, "Failed to open the file\n");
		exit(1);
	}


	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &handler;
	if ((sigaction(SIGUSR1, &sa, NULL) == -1) || (sigaction(SIGUSR2, &sa, NULL) == -1) || (sigaction(SIGCHLD, &sa, NULL) == -1)){
		fprintf(stderr, "Failed to set sigaction\n");
		exit(1);
	}

	memset(&sa_2, 0, sizeof(sa_2));
	sa_2.sa_handler = &int_handler;
	if ((sigaction(SIGINT, &sa_2, NULL) == -1) ){
		fprintf(stderr, "Failed to set sigaction\n");
		exit(1);
	}

	if( (sigemptyset(&maskusr) == -1) || (sigaddset(&maskusr, SIGUSR1) == -1) || (sigaddset(&maskusr, SIGUSR2) == -1)/* || (sigaddset(&maskusr, SIGCHLD) == -1)*/ ){
		fprintf(stderr, "Failed to create signal mask\n");
		exit(1);
	}
	if( sigprocmask(SIG_BLOCK, &maskusr, &oldmask) == -1){
		fprintf(stderr, "Failed to change mask\n");
		exit(1);
	}

	for(i=0; i<8; i++){
		c[i] = fork();
		switch(c[i]){
			case -1:
				fprintf(stderr, "Failed to create a child process\n");
				exit(1);
			case 0:
				child_p(i, &oldmask);
				exit(0);
			default:
				break;
		}
	}

	////////////////////////
	//    PARENT FIELD    //
	////////////////////////

	// Check for round 1 result
	child_exit_status = 0;
	for(i=0; i<8; i++){
		kill(c[i], SIGUSR1);
		sigsuspend(&oldmask);
		if(child_exit_status != 0){
			err_occured = TRUE;
			c[i] = -1;
			child_exit_status = 0;
		}
	}

	if(err_occured){
		// Tell remaining children to terminate
		send_siguser2_to_exit(c, &oldmask);
	}

	// Parent calculates avg error
	sum = 0;
	rewind(fp);
	for( i=0; i<TOTAL_ROW && err_occured == FALSE; i++){
		buffp = fgets(line_buffer, MAX_ROWLENGTH, fp);
		if(buffp == NULL){
			fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
			err_occured = TRUE;
			fclose(fp);
		}
		else{
			if(calculate_error_from_line(&sum, line_buffer, 1) == -1){
				fprintf(stderr, "Something went wrong while calculating error\n");
				err_occured = TRUE;
				fclose(fp);
			}
		}
	}
	avg = sum / ((float) TOTAL_ROW);
	printf("Error of polynomial of degree 5: %.1f\n", avg);


	// Send signal to start round 2
	for(i=0; i<8; i++){
		kill(c[i], SIGUSR1);
		sigsuspend(&oldmask);
	}

	// Check for round 2 result
	child_exit_status = 0;
	for(i=0; i<8; i++){
		kill(c[i], SIGUSR1);
		sigsuspend(&oldmask);
		if(child_exit_status != 0){
			err_occured = TRUE;
			c[i] = -1;
			child_exit_status = 0;
		}
	}


	if(err_occured){
		// Tell remaining children to terminate
		send_siguser2_to_exit(c, &oldmask);
	}
	else{
		// Let all children to know their job has done
		for(i=0; i<8; i++){
			kill(c[i], SIGUSR1);
			sigsuspend(&oldmask);
		}
	}


	// Parent calculates avg error for round 2
	sum = 0;
	fflush(fp);
	rewind(fp);
	for( i=0; i<TOTAL_ROW && err_occured == FALSE; i++){
		buffp = fgets(line_buffer, MAX_ROWLENGTH, fp);
		if(buffp == NULL){
			fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
			err_occured = TRUE;
			fclose(fp);
		}
		else{
			if(calculate_error_from_line(&sum, line_buffer, 2) == -1){
				fprintf(stderr, "Something went wrong while calculating error\n");
				err_occured = TRUE;
				fclose(fp);
			}
		}
	}
	avg = sum / ((float) TOTAL_ROW);
	printf("Error of polynomial of degree 6: %.1f\n", avg);

	fclose(fp);
	return 0;
}


int child_p(int row, sigset_t *mask){
	
	char row_buffers[TOTAL_ROW][MAX_ROWLENGTH] = { "", "", "", "", "", "", "", ""};
	struct flock lock;
	float _x[TOTAL_ROW];
	float _y[TOTAL_ROW];
	float coffs[7] = { 0,0,0,0,0,0,0};
	float result;
	int i;

	is_child = TRUE;

	memset(&lock, 0, sizeof(lock));
	// locking file to sync file among the processes
	lock.l_type = F_WRLCK;
	if( fcntl(fileno(fp), F_SETLKW, &lock) == -1){
		error_in_child(mask, "", TRUE);
	}
	lock.l_type = F_UNLCK;

	read_lines_to_buffers(row_buffers, &lock, mask);
	read_coordinates_from_line(row_buffers[row], _x, _y,  &lock, mask);

	if(lagrange_interpolating(&result, 5, _x[7], _x, _y) == -1){
		fcntl(fileno(fp), F_SETLKW, &lock);
		error_in_child(mask, "Division to 0 is not possible!", FALSE);
	}

	write_result_to_end_of_line(row_buffers[row], result, &lock, mask);

	rewind(fp);
	for( i=0; i<TOTAL_ROW; i++){
		if ( fprintf(fp, "%s", row_buffers[i]) < 0){
			fcntl(fileno(fp), F_SETLKW, &lock);
			error_in_child(mask, "", TRUE);
		}
	}
	fflush(fp);
	//unlocking the file
	if( fcntl(fileno(fp), F_SETLKW, &lock) == -1){
		error_in_child(mask, "", TRUE);
	}


	// wait for parent to ask
	sigsuspend(mask);
	// answer as round 1 finished
	kill(getppid(), SIGUSR1);
	// wait for the next command of parent.
	sigsuspend(mask);
	if(err_occured){
		fclose(fp);
		exit(1);
	}
	// report back as child starting to round 2
	kill(getppid(), SIGUSR1);

	// round 2
	lock.l_type = F_WRLCK;
	if(fcntl(fileno(fp), F_SETLKW, &lock) == -1){
		error_in_child(mask, "", TRUE);
	}
	lock.l_type = F_UNLCK;
	read_lines_to_buffers(row_buffers, &lock, mask);
	read_coordinates_from_line(row_buffers[row], _x, _y,  &lock, mask);

	result = 0;
	if (lagrange_interpolating(&result, 6, _x[7], _x, _y) == -1){
		fcntl(fileno(fp), F_SETLKW, &lock);
		error_in_child(mask, "Division to 0 is not possible!", FALSE);
	}
	write_result_to_end_of_line(row_buffers[row], result, &lock, mask);


	rewind(fp);
	for( i=0; i<TOTAL_ROW; i++){
		if ( fprintf(fp, "%s", row_buffers[i]) < 0){
			fcntl(fileno(fp), F_SETLKW, &lock);
			error_in_child(mask, "", TRUE);
		}
	}
	fflush(fp);
	//unlocking the file
	if(fcntl(fileno(fp), F_SETLKW, &lock) == -1){
		error_in_child(mask, "", TRUE);
	}
	fclose(fp);

	get_coffs(7, _x, _y, coffs);
	printf("Polynomial %d:", row);
	for(i = 6; i>=0; i--){
		printf("%.1f  ", coffs[i]);
	}
	

	// wait for parent to ask
	sigsuspend(mask);
	// answer as round 2 finished
	kill(getppid(), SIGUSR1);
	// wait for parent to say terminate
	sigsuspend(mask);
	// terminate
	return 0;
}



void read_lines_to_buffers(char row_buffers[TOTAL_ROW][MAX_ROWLENGTH], struct flock *lock, sigset_t *mask){
	int i;
	char* buffp;
	rewind(fp);
	
	for( i=0; i<TOTAL_ROW; i++){
		buffp = fgets(row_buffers[i], MAX_ROWLENGTH, fp);
		if(buffp == NULL){
			fcntl(fileno(fp), F_SETLKW, lock);
			error_in_child(mask, "", TRUE);
			fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
		}
		else if (buffp[strlen(buffp)-1] != '\n'){
			fcntl(fileno(fp), F_SETLKW, lock);
			error_in_child(mask, "ERROR: A Row has more than Max Length of a row.", FALSE);
		}
	}
}

void read_coordinates_from_line(char* str, float _x[TOTAL_ROW], float _y[TOTAL_ROW], struct flock *lock, sigset_t *mask){
	int numberof_read;

	numberof_read = sscanf( str, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", 
					&_x[0], &_y[0],
					&_x[1], &_y[1],
					&_x[2], &_y[2],
					&_x[3], &_y[3],
					&_x[4], &_y[4],
					&_x[5], &_y[5],
					&_x[6], &_y[6],
					&_x[7], &_y[7]);
	if(numberof_read != (TOTAL_ROW * 2)){
		// an error occured while values from buffer
		fcntl(fileno(fp), F_SETLKW, lock);
		error_in_child(mask, "ERROR: A row is in wrong structure!", FALSE);
	}

}

void write_result_to_end_of_line(char line[MAX_ROWLENGTH], float result, struct flock *lock, sigset_t *mask){
	int numberof_read;
	char result_str[20];

	numberof_read = snprintf(result_str, 20, "%.1f\n", result);
	if(numberof_read >= 20 || (strlen(line) + numberof_read) >= MAX_ROWLENGTH){
		// an error occured while converting float value to string
		fcntl(fileno(fp), F_SETLKW, lock);
		error_in_child(mask, "ERROR: A float values is too long to write!", FALSE);
	}

	line[strlen(line)-1] = ',';
	strcat(line, result_str);

}


int lagrange_interpolating(float *result, int degree, float fx, float _x[TOTAL_ROW], float _y[TOTAL_ROW]){

	int i, j, k;
	float mid_res;
	*result = 0;

	for( i=0; i< (degree+1); i++){
		mid_res=1;

		for( j=0; j< (degree+1); j++){
			if(i!=j){
				if( (k = _x[i]-_x[j])  == 0){
					return -1;
				}
				mid_res = mid_res * (fx-_x[j]) / k;
			}
		}
		*result = *result + (mid_res * _y[i]);
	}


	return 0;
}

int get_coffs(int degree, float _x[TOTAL_ROW], float _y[TOTAL_ROW], float _coffs[degree+1]){
	int i,j,k;
	float _t_coffs[TOTAL_ROW];

	for(i=0; i<degree; i++){

		for(j=0; j<degree; j++){
			_t_coffs[j] = 0;
		}

		if(i == 0){
			_t_coffs[0] = -_x[1] / (_x[i] - _x[1]);
			_t_coffs[1] = 1 / (_x[i] - _x[1]);
			j = 2;
		}
		else{
			_t_coffs[0] = -_x[0] / (_x[i] - _x[0]);
			_t_coffs[1] = 1 / (_x[i] - _x[0]);
            j = 1;
    	}

		for(; j < degree; j++) {
			if (i != j){
				for ( k = degree-1; k>=1; k--) {
					_t_coffs[k] = _t_coffs[k] * (-_x[j] / (_x[i]-_x[j])) + _t_coffs[k-1] / (_x[i]-_x[j]);
				}
				_t_coffs[0] = _t_coffs[0] * (-_x[j] / (_x[i] - _x[j]));
			}
		}    
		for( k =0; k<degree; k++){
			_coffs[k] += _y[i] * _t_coffs[k];
		}
	}

	return 0;
}


int calculate_error_from_line(float* sum, char line[MAX_ROWLENGTH], int round){

	int len;
	int i;
	int comma_cnt=0;
	int read_cnt;
	float fx, px, temp;

	if(round !=1 && round != 2){
		return -1;
	}

	len = strlen(line);
	for( i = len-1; i>=0 && comma_cnt <= round; i--){
		if(line[i] == ','){
			comma_cnt++;
		}
	}
	if(comma_cnt != round+1){
		return -1;
	}
	i += 2;

	if(round == 1){
		read_cnt = sscanf( &line[i], "%f,%f", &fx, &px);
	}
	else if(round == 2){
		read_cnt = sscanf( &line[i], "%f,%f,%f", &fx, &temp, &px);
	}

	if(read_cnt != (round+1)){
		return -1;
	}

	(*sum) += fabs(fx - px);

	return 0;
}

void error_in_child(sigset_t *mask, char* str,  int prnterrno){
	if(prnterrno){
		fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
	}
	else{
		fprintf(stderr, "%s\n", str);
	}
	sigsuspend(mask);
	fclose(fp);
	exit(1);
}


void send_siguser2_to_exit(pid_t c[], sigset_t *mask){
	int i;

	for(i=0; i<8; i++){
		if(c[i] != -1){
			kill(c[i], SIGUSR2);
			sigsuspend(mask);
		}
	}
	fclose(fp);
	exit(1);
}

void int_handler(int dummy){
	fclose(fp);

	if( !is_child){
		printf("Program interupted by user!\n");
	}
	exit(1);
}
