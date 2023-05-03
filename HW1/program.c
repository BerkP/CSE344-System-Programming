#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>

#define TRUE 1
#define FALSE 0

sig_atomic_t sgnl_flg = 0;

void handler(int dummy){
	sgnl_flg = 1;
}


struct dir_node{
	char *file_name;				// filename string
	struct dir_node **sub_files;	// node pointer array
	int numberof_sf; 				// number of sub files
	int cap;						// capacity
};
typedef struct dir_node dir_node;

struct file_opts{
	char* path;
	char* name;
	off_t size;
	mode_t type;
	char* perms;
	nlink_t links;
};
typedef struct file_opts file_opts;

int parse_command_line_opt(int , char *const *, file_opts *);
int traverse_directories(char *, file_opts *, dir_node *);
int is_searched_file(char *, file_opts *, struct stat *);


mode_t get_mode_of_opt_type(char* );
int is_str_a_int(char *);
void free_traverse_resources(char *, DIR *, dir_node *);
int cmpstr_plus(char* , char*);
char* get_perm_string_of_file(struct stat *);


dir_node *create_new_dir_node(char* );
int add_sub_file_to_node(dir_node *, dir_node *);
void print_dir_node(dir_node *, int );
void print_dir_tree(dir_node *);
void free_the_tree(dir_node *);


int main(int argc, char *const *argv)
{
	
	struct sigaction sa;
	memset(&sa,0,sizeof(sa));
    sa.sa_handler = &handler;
    sigaction(SIGINT, &sa, NULL);
	
	int cl_parsed;
	int searchresult;
	dir_node *root;

	file_opts *searchfile = malloc(sizeof(file_opts));
	if(searchfile == NULL){
		fprintf(stderr, "An error occured while allocating memory!\n");
		return 1;
	}
	cl_parsed = parse_command_line_opt(argc, argv, searchfile);
	if(cl_parsed == -1){
		free(searchfile);
		return 1;
	}
	root = create_new_dir_node(searchfile->path);
	if(root == NULL){
		fprintf(stderr, "An error occured while allocating memory!\n");
		free(searchfile);
		return 1;
	}

	searchresult = traverse_directories(searchfile->path, searchfile,root);
	if(searchresult == 0){
		printf("No file found.\n");
	}
	else if(searchresult == 1){
		print_dir_tree(root);
	}
	
	free_the_tree(root);
	free(searchfile);


	// signal handled in the functions (traverse / print)
	if(sgnl_flg == 1){
		printf("Execution interrupted by user!\n");
	}
	if(searchresult == -1 || sgnl_flg){
		//in case of error or interrupt
		return 1;
	}
	else{
		return 0;
	}
}

int parse_command_line_opt(int argc, char *const *argv, file_opts *searchfile){
	int ch;
	int pathgiven = FALSE;
	int opscount = 0;

	searchfile->path = NULL;
	searchfile->name = NULL;
	searchfile->perms = NULL;
	searchfile->size = -1;
	searchfile->type = -1;
	searchfile->links = -1;

	while((ch = getopt(argc,argv,"w:f:b:t:p:l:")) != -1){
		switch(ch){

			case 'w':
				searchfile->path = optarg;
				pathgiven = TRUE;
				break;

			case 'f':
				searchfile->name = optarg;
				opscount++;
				break;

			case 'b':
				if(is_str_a_int(optarg) == TRUE){
					searchfile->size = atoi(optarg);
					opscount++;
				}
				else{
					fprintf(stderr,"Please use digits for number of bits.\n");
					return -1;
				}
				break;

			case 't':
				if ((searchfile->type = get_mode_of_opt_type(optarg)) != -1){
					opscount++;
				}
				else{
					fprintf(stderr,"Please enter valid type of file.\n");
					return -1;
				}
				break;

			case 'p':
				if(strlen(optarg) == 9){
					searchfile->perms = optarg;
					opscount++;
				}
				else{
					fprintf(stderr,"Please enter 9 characters for permissions.\n");
					return -1;
				}
				break;

			case 'l':
				if(is_str_a_int(optarg) == TRUE){
					searchfile->links = atoi(optarg);
					opscount++;
				}
				else{
					fprintf(stderr,"Please use digits for number of links.\n");
					return -1;
				}
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
	else if (pathgiven == FALSE){
		fprintf(stderr,"Please enter path(-w) for correct usage.\n");
		return -1;
	}
	else if (opscount < 1){
		fprintf(stderr,"Please enter at least one criteria for correct usage.\n");
		return -1;
	}

	return 0;
}


int traverse_directories(char *path, file_opts *searchfile, dir_node *dnode){
	int r;
	int found;
	int newpathsize;
	int searchresult;
	int recursiveresult;
	char* newpath;
	DIR *currentdir;
	struct stat filestat;
	struct dirent *currentdirent;
	dir_node *currnode;

	found = FALSE;
	currentdir = opendir(path);
	
	if(currentdir == NULL){
		fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	errno = 0;

	while((currentdirent = readdir(currentdir)) != NULL){
		recursiveresult = 0;
		if ( strcmp(currentdirent->d_name, ".") == 0 || strcmp(currentdirent->d_name,"..") == 0 ){
			continue;
		}
		
		if(sgnl_flg == 1){
			closedir(currentdir);
			return -1;
		}

		newpathsize = strlen(path) + strlen(currentdirent->d_name) + 1;
		newpath = malloc((newpathsize + 1) * sizeof(char));
		currnode = create_new_dir_node(currentdirent->d_name);

		if(newpath == NULL || currnode == NULL){
			fprintf(stderr, "An error occured while allocating memory!\n");
			free_traverse_resources(newpath, currentdir, currnode);
			return -1;
		}

		strcpy(newpath, path);
		strcat(newpath, "/");
		strcat(newpath, currentdirent->d_name);


		r = lstat(newpath, &filestat);
		if (r == -1){
			fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
			free_traverse_resources(newpath, currentdir, currnode);
			return -1;
		}

		searchresult = is_searched_file(currentdirent->d_name, searchfile, &filestat);

		if(sgnl_flg == 1){
			free_traverse_resources(newpath, currentdir, currnode);
			return -1;
		}

		if (S_ISDIR(filestat.st_mode)){
			recursiveresult = traverse_directories(newpath, searchfile, currnode);
			if(recursiveresult == -1){
				free_traverse_resources(newpath, currentdir, currnode);
				return -1;
			}
		}

		if(searchresult > 0 || recursiveresult == 1){
			found = TRUE;
			if (add_sub_file_to_node(dnode, currnode) == -1){
				fprintf(stderr, "An error occured while allocating memory!\n");
				free_traverse_resources(newpath, currentdir, currnode);
			return -1;
			}
		}
		else{
			free_the_tree(currnode);
		}
		free(newpath);
		errno = 0;
	}
	// checking errors for closedir() and readdir()
	if( closedir(currentdir) == -1 || errno != 0){
		fprintf(stderr, "ERRNO: %d - %s \n", errno, strerror (errno));
		return -1;
	}

	return found;
}

int is_searched_file(char *filename, file_opts *searchfile, struct stat *filestat){
	
	int matchcount = 0;
	char* perms;

	if(searchfile->name != NULL && cmpstr_plus(searchfile->name, filename) == 0 ){
		return 0;
	}
	matchcount++;

	if(searchfile->size != -1 && searchfile->size != filestat->st_size){
		return 0;
	}
	matchcount++;

	if(searchfile->type != -1 && (filestat->st_mode & S_IFMT) != searchfile->type){
		return 0;
	}
	matchcount++;

	if(searchfile->links != -1 && searchfile->links != filestat->st_nlink){
		return 0;
	}
	matchcount++;

	perms = get_perm_string_of_file(filestat);
	if(searchfile->perms != NULL && strcmp(perms, searchfile->perms) != 0 ){
		free(perms);
		return 0;
	}
	free(perms);
	matchcount++;

	return matchcount;	

}


/*----------------------------*/
/*          HELPERS           */
/*----------------------------*/


mode_t get_mode_of_opt_type(char* optarg){
	char t;

	if (strlen(optarg) != 1){
		return -1;
	}
	t = optarg[0];
	switch(t){
		case 'd':
			return S_IFDIR;
		case 's':
			return S_IFSOCK;
		case 'b':
			return S_IFBLK;
		case 'c':
			return S_IFCHR;
		case 'f':
			return S_IFREG;	
		case 'p':
			return S_IFIFO;
		case 'l':
			return S_IFLNK;
		default:
			return -1;
	}
}

int is_str_a_int(char *str){
	int i;
	for(i=0; str[i] != '\0'; i++){
		if(isdigit(str[i]) == 0)
			return FALSE;
	}
	return TRUE;
}


void free_traverse_resources(char *path, DIR *dir, dir_node *currnode){
	closedir(dir);
	free(path);
	free_the_tree(currnode);
}



char* get_perm_string_of_file(struct stat *filestat){

	char* perms = malloc(10 * sizeof(char));

	perms[0] = (filestat->st_mode & S_IRUSR) ? 'r' : '-';
	perms[1] = (filestat->st_mode & S_IWUSR) ? 'w' : '-';
	perms[2] = (filestat->st_mode & S_IXUSR) ? 'x' : '-';

	perms[3] = (filestat->st_mode & S_IRGRP) ? 'r' : '-';
	perms[4] = (filestat->st_mode & S_IWGRP) ? 'w' : '-';
	perms[5] = (filestat->st_mode & S_IXGRP) ? 'x' : '-';

	perms[6] = (filestat->st_mode & S_IROTH) ? 'r' : '-';
	perms[7] = (filestat->st_mode & S_IWOTH) ? 'w' : '-';
	perms[8] = (filestat->st_mode & S_IXOTH) ? 'x' : '-';
	perms[9] = '\0';

	return perms;
}


/* Compare String with Plus Regular Expression */
/* reg can have + regular expression */
int cmpstr_plus(char* reg, char* str){
    int i=0;
    int k=0;
    int cnt;

    while (reg[i] != '\0' || str[k] != '\0'){

        if(reg[i] == '+' && i != 0){
            cnt = 1;
            while(tolower(reg[i-1]) == tolower(str[k])){
                k++;
                cnt++;
            }
            if( tolower(reg[i+1]) == tolower(str[k-1]) && cnt > 1){
                reg++;
            }
            i++;
        }
        else if(tolower(reg[i]) == tolower(str[k])){
            i++;
            k++;
        }
        else
            return 0;
    }

    if(reg[i] == '\0' || str[k] == '\0')
        return 1;
    else
        return 0;

}



/*----------------------------*/
/*      TREE STRUCTURE        */
/*----------------------------*/


/* Returns NULL if allocating failed */
dir_node *create_new_dir_node(char* file_name){
	dir_node *new_node = malloc(sizeof(dir_node));

	if( new_node == NULL){
		/* Caller function has to handle this error */
		return NULL;
	}

	new_node->file_name = malloc((strlen(file_name) + 1) * sizeof(char));
	strcpy(new_node->file_name, file_name);
	new_node->numberof_sf = 0;
	new_node->cap = 0;
	new_node->sub_files = NULL;

	return new_node;
}

void print_dir_node(dir_node *node, int level){
	int i;

	if(sgnl_flg == 1){
		return;
	}

	printf("|");
	for(i=0; i < level; i++){
		printf("--");
	}
	printf("%s\n", node->file_name);

	for(i=0; i < node->numberof_sf; i++){
		if(sgnl_flg == 1){
			return;
		}
		print_dir_node(node->sub_files[i], level + 1);
	}
}

void print_dir_tree(dir_node *root){
	int i;

	printf("%s\n", root->file_name);
	for(i=0; i < root->numberof_sf; i++){
		if(sgnl_flg == 1){
			return;
		}
		print_dir_node(root->sub_files[i], 1);
	}

}

void free_the_tree(dir_node *node){
	int i;

	if(node == NULL){
		return;
	}
	for(i=0; i < node->numberof_sf; i++){
		free_the_tree(node->sub_files[i]);
	}
	free(node->file_name);
	free(node->sub_files);
	free(node);

}

/* Returns 0 when success. Returns -1 when error occurs */
int add_sub_file_to_node(dir_node *node, dir_node *sub_file){
	if(node->cap == 0){
		node->cap = 1;
		node->sub_files = malloc(node->cap * sizeof(dir_node*));
	}
	else if (node->numberof_sf == node->cap){
		node->cap *= 2;
		node->sub_files = realloc(node->sub_files, (node->cap * sizeof(dir_node*)));
	}

	if(node->sub_files == NULL){
		return -1;
	}
	node->sub_files[node->numberof_sf] = sub_file;
	node->numberof_sf++;
	return 0;
}