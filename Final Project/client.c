#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <time.h>


#define TRUE 	1
#define FALSE 	0
#define BUFFER_LEN 256
#define ENTRY_SIZE 128


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

database_t db;
char timestamp[10];

int create_db();
void free_db();
int get_response(int socketfd, int);
int fill(int socketfd);
int print_table(column_t *cols, int size);
int get_query_start_index(char* );
int parse_command_line_opt(int , char **, char** , char** , int* , int* );
int is_str_a_int(char *);
char* get_timestamp();



int main(int argc, char **argv){
	FILE *fp;
	char *ip;
	char *queryfile;
	int port, id, socketfd, temp_id, t_errno, querycount;
	char *buffp;
	struct sockaddr_in server_a; 
	char buffer[BUFFER_LEN];
	char line_buff[BUFFER_LEN];


	if(parse_command_line_opt(argc, argv, &queryfile, &ip, &port, &id) == -1){
		fprintf(stderr,"(%s) Correct usage:\n\t./client –i id(integer) -a 127.0.0.1 -p PORT(integer) -o pathToQueryFile \n", get_timestamp());
		exit(EXIT_FAILURE);
	}

	if((fp = fopen(queryfile, "r")) == NULL){
		fprintf(stderr, "(%s) Querry File Error - %s \n", get_timestamp(), strerror (errno));
		return -1;
	}

	if( (socketfd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
		fprintf(stderr, "(%s) Socket error - %s  \n", get_timestamp(), strerror(errno));
		fclose(fp);
		exit(EXIT_FAILURE);
	}

	server_a.sin_family = AF_INET; 
	server_a.sin_port = htons(port);
	server_a.sin_addr.s_addr = inet_addr(ip);

	printf("(%s) Client-%d connecting to %s:%d!\n", get_timestamp(), id, ip, port);

	if(connect(socketfd, (struct sockaddr *)&server_a, sizeof(server_a)) == -1){
		fprintf(stderr, "(%s) Connect error - %s  \n", get_timestamp(), strerror(errno));
		fclose(fp);
		exit(EXIT_FAILURE);
	}
	memset(buffer, 0 , BUFFER_LEN);
	t_errno = errno;
	querycount = 0;

	while((buffp = fgets(line_buff, BUFFER_LEN, fp)) != NULL){
		sscanf(line_buff,"%d",&temp_id);
		if(temp_id == id){
			//send query
			querycount++;
			strcpy(buffer,&line_buff[get_query_start_index(line_buff)]);
			buffer[strlen(buffer)-1] = '\0';
			printf("(%s) Client-%d connected and sending query ‘%s’\n", get_timestamp(), id, buffer);
			if (write(socketfd, buffer, BUFFER_LEN) == -1){
				fprintf(stderr, "(%s) Write error - %s  \n", get_timestamp(), strerror(errno));
				fclose(fp);
				close(socketfd);
				exit(EXIT_FAILURE);
			}

			//get response
			if( get_response(socketfd, id) == -1){
				fclose(fp);
				sprintf(buffer, "end");
				write(socketfd, buffer, BUFFER_LEN);
				close(socketfd);
				exit(EXIT_FAILURE);
			}
		}
	}
	fclose(fp);
	if(errno != t_errno){
		fprintf(stderr, "(%s) Query File Error - %s \n", get_timestamp(), strerror (errno));
		close(socketfd);
		return -1;
	}

	sprintf(buffer, "end");
	write(socketfd, buffer, BUFFER_LEN);
	close(socketfd);
	printf("A total of %d queries were executed, client is terminating.\n", querycount);
	return 0;
}

int get_response(int socketfd, int id){
	char buffer[BUFFER_LEN];
	int col, row;
	struct timeval begin, end;
	gettimeofday(&begin, NULL);
	double responsetime;
	if (read(socketfd, &col, sizeof(int)) == -1){
		fprintf(stderr, "(%s) Read Error - %s \n", get_timestamp(), strerror (errno));
		return -1;
	}

	if(col == 0){
		if(read(socketfd, &buffer, BUFFER_LEN) == -1){
			fprintf(stderr, "(%s) Read Error - %s \n", get_timestamp(), strerror (errno));
			return -1;
		}
		gettimeofday(&end, NULL);
		responsetime = (double)((((end.tv_sec - begin.tv_sec) * 1000000) + end.tv_usec) - (begin.tv_usec)) / 1000000;
		printf("(%s) Server’s response to Client-%d is %d records, and arrived in %lf seconds.\n", get_timestamp(), id, row-1, responsetime);
		printf("(%s) Server’s response: %s\n", get_timestamp(), buffer);
		return 0;
	}

	if (read(socketfd, &row, sizeof(int)) == -1){
		fprintf(stderr, "(%s) Read Error - %s \n", get_timestamp(), strerror (errno));
		return -1;
	}

	db.columnsize = col;
	db.rowsize = row;
	
	if(create_db() == -1){
		free_db();
		exit(EXIT_FAILURE);
	}
	if(fill(socketfd) == -1){
		free_db();
		exit(EXIT_FAILURE);
	}
	gettimeofday(&end, NULL);
	responsetime = (double)((((end.tv_sec - begin.tv_sec) * 1000000) + end.tv_usec) - (begin.tv_usec)) / 1000000;
	printf("(%s) Server’s response to Client-%d is %d records, and arrived in %lf seconds.\n", get_timestamp(), id, row-1, responsetime);
	print_table(db.columns, db.columnsize);
	free_db();
	return 0;
}

int fill(int socketfd){

	int i, j, size;

	for(i = 0; i < db.columnsize; i++){
		for(j = 0; j < db.rowsize; j++){
			if((size = read(socketfd, db.columns[i].entries[j], ENTRY_SIZE)) == -1 || size != ENTRY_SIZE){
				fprintf(stderr, "(%s) Read socket error: %s \n", get_timestamp(), strerror (errno));
				return -1;
			}
		}
	}
	return 0;
}

int print_table(column_t *cols, int size){
	int i, k;

	for(i = 0; i < db.rowsize; i++){
		for(k = 0; k < size; k++){
			printf("%-10.10s    ", cols[k].entries[i]);
		}
		printf("\n");
	}

	return 0;
}

int create_db(){
	int i,j;

	db.columns = malloc(db.columnsize * sizeof(column_t));
	if(db.columns == NULL){
		fprintf(stderr, "Allocation error: %s\n", strerror(errno));
		return -1;
	}

	for(i = 0; i < db.columnsize; i++){
		db.columns[i].entries = malloc(db.rowsize * sizeof(char*));
		if(db.columns[i].entries == NULL){
			fprintf(stderr, "Allocation error: %s\n", strerror(errno));
			return -1;
		}

		for(j = 0; j < db.rowsize; j++){
			db.columns[i].entries[j] = malloc(ENTRY_SIZE * sizeof(char));
			if(db.columns[i].entries[j] == NULL){
				fprintf(stderr, "Allocation error: %s\n", strerror(errno));
				return -1;
			}
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

int get_query_start_index(char* str){
	int i = 0;
	while(str[i] != ' ' && str[i] != '\0'){
		i++;
	}

	if(str[i] == ' '){
		return ++i;
	}
	else{
		return -1;
	}
}

int parse_command_line_opt(int argc, char **argv, char** queryfile, char** ip, int* port, int* id){
	int ch;
	int opscount = 0;
	while((ch = getopt(argc,argv,"p:i:a:o:")) != -1){
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
				(*queryfile) = optarg;
				opscount++;
				break;

			case 'i':
				if(is_str_a_int(optarg) == TRUE){
					(*id) = atoi(optarg);
					opscount++;
				}
				else{
					fprintf(stderr,"Please use integer for id.\n");
					return -1;
				}
				break;

			case 'a':
				(*ip) = optarg;
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

char* get_timestamp(){
	time_t time_;
	struct tm *time_s;
	time_ = time(NULL);
	time_s = localtime(&time_);
	sprintf(timestamp, "%02d:%02d:%02d", time_s->tm_hour, time_s->tm_min, time_s->tm_sec);
	return timestamp;
}