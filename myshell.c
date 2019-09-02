#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <setjmp.h>

#define CHILD_MAX 1024

typedef struct cmd_struct {
	
	char **args;	//buffer of arguments to be used in exec() call
	int pipe_fd_r;	//read file descriptor returned from pipe()
	int pipe_fd_w;	//write file descriptor returned from pipe()
	char *file_in;	//filename for input redirection
	char *file_out;	//filename for output redirection
	char *file_err;	//filename for error redirection
	pid_t pgid;	//process group id, used for running background processes
	
} cmd_st;

pid_t pgid_counter = 0;
pid_t pgid_curr;
pid_t returned;

pid_t pid_buf[CHILD_MAX];
int pid_idx = 0;

char *cmd;
cmd_st *cmd_buf;
int cmd_num = 0;

sigjmp_buf sigint_env;


/**
* handler() is called when a SIGINT (or ^C) occurs while myshell is running.
* The function kills all foreground child processes currently running, before using a siglongjmp() to go to the start of the myshell() loop.
**/
void handler(int sig){
	
	if (sig == SIGINT) {

		int i = 0;
		for (; i <= pid_idx; i++){
			if (getpid() == pid_buf[i]) {
				kill(pid_buf[i], sig);
				pid_buf[i] = 0;
			}
		}

		cleanup();

		printf("\n");

		siglongjmp(sigint_env, 1);
	}
}


/**
* myshell() represents the main loop of the program.
* It takes input from stdin and passes it to a parser that fills a queue of command structs, then calls a helper function to execute the commands.
**/
void myshell(){
	
	char *input;

	do {

		/* We jump here after handling SIGINT, to reset buffers and restart the myshell loop. */
		while(sigsetjmp(sigint_env, 1) != 0) {
	
			/* Call cleanup function everytime we reset the myshell() loop. */
			cleanup();
		}

		/* Reap all zombie processes. */
		while(waitpid(-1, NULL, WNOHANG) > 0);


		/**
		* Preprocessing and error checking on input.
		**/

		cmd_buf = calloc(1, sizeof(cmd_st));

		/* Input from terminal. */
		if (isatty(0)) {		
			input = readline("myshell> ");
		}
		
		/* Input from file. */
		else
			input = readline(NULL);

		/* Exit on ^D character. */
		if (input == NULL) {
			printf("^D\n");
			exit(EXIT_SUCCESS); 
		}

		/* Add non-empty commands to readline history so users can use up/down arrow keys to retrieve past commands. */
		if (strlen(input) > 0)
			add_history(input);

		/* On empty input, restart myshell() loop with new prompt. */
		else {
			free(input);
			input = NULL;
			continue;
		}

		/* Parse input and store commands in queue. */
		parseInput(input);

		/* Execute all commands that have been stored in queue. */
		runCommands();

		/* Clear out memory for next iteration of myshell() loop. */
		cleanup();

	/* Repeats forever when running from a terminal, will break if reading from file. */
	} while (isatty(0));

	exit(EXIT_SUCCESS);
}


/**
* trimWhitespace() removes leading whitespace.
**/
char* trimWhitespace(char*str){
	while(isspace((unsigned char)*str)) str++;
	return str;
}


/**
* split() tokenizes strings on whitespace, allowing us to create an array of arguments to pass via exec().
**/
#define DELIM " \t\r\n\a"
char** split(char* input){

	int current = 0;
	char * str;
	int toks_size = 2;
	char ** toks = malloc(sizeof(char*) * toks_size);
	int error;
	
	if (!toks){
	    perror("Error on split");
	    siglongjmp(sigint_env, 1);
	}

	str = strtok(input, DELIM);
	while(str) {

		if (current * 4 >= toks_size) {
			toks_size *= 4;
			if((toks = realloc(toks, toks_size * sizeof(*toks))) == NULL){
				perror("Error on split");			
				exit(EXIT_FAILURE);
			}		
		}

		toks[current] = str;
		current++;
		str = strtok(NULL, DELIM);
	}

	toks[current] = '\0';
	return toks;
}


/**
* parseInput() is the main body for a parser that takes input from stdin and tokenizes based on a variety of control characters.
* Semicolons and ampersands are handled here, while pipes and file redirection are handled in their own helper functions.
**/
void parseInput(char *input) {

	char *str_semi, *str_bg;
	char *tok_semi, *saveptr;

	/* Parse on semicolons. Repeat until no more semicolons are found. */
	for(str_semi = input; ; str_semi = NULL) {

		tok_semi = strtok_r(str_semi, ";", &saveptr); //We use strtok_r() to avoid weird errors with strtok() not saving endptr correctly
		if (tok_semi == NULL)
	           	break;

		cmd_buf = realloc(cmd_buf, sizeof(cmd_st) * (cmd_num + 1));
		cmd_buf[cmd_num].args = NULL;
		cmd_buf[cmd_num].pipe_fd_w = NULL;
		cmd_buf[cmd_num].pipe_fd_r = NULL;
		cmd_buf[cmd_num].file_in = NULL;
		cmd_buf[cmd_num].file_out = NULL;
		cmd_buf[cmd_num].file_err = NULL;
		cmd_buf[cmd_num].pgid = NULL;

		char *tok_bg;
		str_bg = tok_semi;
		int bg_flag;
		
		/* Parse on ampersands. */
		do {
			bg_flag = 0;				
			pid_t pgid = 0;


			/* & found, create new entry in cmd_buf for potential right side of &, then parse left side. */
			if ((tok_bg = strstr(str_bg, "&")) != NULL && *(tok_bg + 1) != '>') {
				char* errorTest = trimWhitespace(str_bg);
				if (tok_bg == input || (*str_bg == ' ' && *errorTest == '&')) {
					printf("myshell: syntax error near unexpected token `&`\n");
					siglongjmp(sigint_env, 1);
				}	
				bg_flag = 1;
				pgid_counter++;
				pgid = pgid_counter;

				cmd_buf = realloc(cmd_buf, sizeof(cmd_st) * (cmd_num + 2));
				cmd_buf[cmd_num + 1].args = NULL;
				cmd_buf[cmd_num + 1].pipe_fd_w = NULL;
				cmd_buf[cmd_num + 1].pipe_fd_r = NULL;
				cmd_buf[cmd_num + 1].file_in = NULL;
				cmd_buf[cmd_num + 1].file_out = NULL;
				cmd_buf[cmd_num + 1].file_err = NULL;
				cmd_buf[cmd_num + 1].pgid = NULL;


				/* Parse left side of &. */
				*tok_bg = '\0';		
				parsePipe(str_bg, pgid);
				str_bg = tok_bg + 1;
			}


			/* No & found, parse input for |. */
			else {
				parsePipe(str_bg, pgid);
			}


			/* Add pgid to command in queue. This will be 0 if the process is to be run in the foreground. */
			cmd_buf[cmd_num].pgid = pgid;
			if(bg_flag && str_bg)	cmd_num++;
			

		/* Repeats until there are no more & characters that aren't attached to file redirection (ie &>). */
		} while(*str_bg && (bg_flag || ((tok_bg = strstr(str_bg, "&")) != NULL && *(tok_bg + 1) != '>')));
		if (!bg_flag)	cmd_num++;
	}

	free(input);
	input = NULL;
}


/**
* parsePipe() is a helper function that looks for | characters in a string.
* If a pipe is found, we flip pipe_flag, call pipe(), add the write file descriptor to the current cmd_st, and parse for redirects.
* We then add a new cmd_st to the queue and add the read file descriptor to the struct. When we iterate again, we will parse the right side for redirects as well.
**/
void parsePipe(char *str_pipe, pid_t pgid) {

	char *tok_pipe;
	int pipe_flag;

	do {	
		pipe_flag = 0;


		/* Pipe found. */
		if (str_pipe && (tok_pipe = strstr(str_pipe, "|")) != NULL) {
		
			char* errorTest = trimWhitespace(str_pipe);
			if (tok_pipe == str_pipe || *(tok_pipe + 1) == NULL || (*str_pipe == ' ' && *errorTest == '|')) {
				printf("myshell: syntax error near unexpected token `|`\n");
				siglongjmp(sigint_env, 1);
			}

			pipe_flag = 1;

			int fd[2];
			pipe(fd);

			cmd_buf[cmd_num].pipe_fd_w = fd[1];


			*tok_pipe = '\0';

			parseRedirect(str_pipe, &cmd_buf[cmd_num]);

			str_pipe = tok_pipe + 1;

			cmd_num++;

			cmd_buf = realloc(cmd_buf, sizeof(cmd_st) * (cmd_num + 2));
			cmd_buf[cmd_num].args = NULL;
			cmd_buf[cmd_num].pipe_fd_w = NULL;
			cmd_buf[cmd_num].pipe_fd_r = NULL;
			cmd_buf[cmd_num].file_in = NULL;
			cmd_buf[cmd_num].file_out = NULL;
			cmd_buf[cmd_num].file_err = NULL;
			cmd_buf[cmd_num].pgid = NULL;

			cmd_buf[cmd_num].pipe_fd_r = fd[0];
			cmd_buf[cmd_num].pgid = pgid;
		}


		/* No pipe found, parse string for redirects. */
		else {
			parseRedirect(str_pipe, &cmd_buf[cmd_num]);
		}

	/* Repeats while str_pipe is valid and we have either just seen a pipe or a pipe is found using strstr(). */
	} while(str_pipe && (pipe_flag || (tok_pipe = strstr(str_pipe, "|")) != NULL));
}


/**
* parseRedirect() is a helper function that handles file redirection using <, >.
* If a redirection character is found, the corresponding filename is parsed and added to the appropriate command struct in the queue.
* The file will be opened and dup2() will be called during the command execution stage.
**/
void parseRedirect(char *str_redirect, cmd_st *command){

	char *ptr_in, *ptr_out;
	char *start_in, *start_out;
	int stdout_ctl, cmd_len, in_len, out_len;

	if (*str_redirect == NULL) {
		printf("myshell: syntax error\n");
		siglongjmp(sigint_env, 1);
	}
		

	ptr_in = strstr(str_redirect, "<");
	ptr_out = strstr(str_redirect, ">");


	/* If there are two of the same redirect characters in the same line (eg ls > out > out2), throw a syntax error. */
	if ((ptr_in && strstr(ptr_in + 1, "<") != NULL) || (ptr_out && strstr(ptr_out + 1, ">") != NULL)){
		printf("myshell: syntax error: there may only be one of each redirection character\n");
		siglongjmp(sigint_env, 1);
	}


	/* If there is both a > and <, but the < appears first. */
	if (ptr_in && ptr_out && ptr_in < ptr_out){		
		cmd_len = ptr_in - str_redirect;
		
		out_len = strlen(str_redirect) - (ptr_out + 1 - str_redirect);
		start_out = ptr_out + 1;

		start_in = ptr_in + 1;

		switch (*(ptr_out - 1)) {

			case '1':
				stdout_ctl = 1;
				ptr_out--;
				break;
			case '2':
				stdout_ctl = 2;
				ptr_out--;
				break;
			case '&':
				stdout_ctl = 3;
				ptr_out--;
				break;
			default:
				stdout_ctl = 1;
				break;
		}

		in_len = ptr_out - 1 - ptr_in;
	}
	

	/* If there is both a > and <, but the > appears first. */
	else if (ptr_in && ptr_out && ptr_out < ptr_in) {	
		in_len = strlen(str_redirect) - (ptr_in + 1 - str_redirect);
		start_in = ptr_in + 1;

		out_len = ptr_in - (ptr_out + 1);
		start_out = ptr_out + 1;

		switch (*(ptr_out - 1)) {

			case '1':
				stdout_ctl = 1;
				ptr_out--;
				break;
			case '2':
				stdout_ctl = 2;
				ptr_out--;
				break;
			case '&':
				stdout_ctl = 3;
				ptr_out--;
				break;
			default:
				stdout_ctl = 1;
				break;
		}

		cmd_len = ptr_out - str_redirect;
	}


	/* If there is only a <. */
	else if (ptr_in) {
		cmd_len = ptr_in - str_redirect;

		in_len = strlen(str_redirect) - (ptr_in + 1 - str_redirect);
		start_in = ptr_in + 1;
	}


	/* If there is only a >. */
	else if (ptr_out) {
		out_len = strlen(str_redirect) - (ptr_out + 1 - str_redirect);
		start_out = ptr_out + 1;

		switch (*(ptr_out - 1)) {

			case '1':
				stdout_ctl = 1;
				ptr_out--;
				break;
			case '2':
				stdout_ctl = 2;
				ptr_out--;
				break;
			case '&':
				stdout_ctl = 3;
				ptr_out--;
				break;
			default:
				stdout_ctl = 1;
				break;
		}

		cmd_len = ptr_out - str_redirect;
	}


	/* If there are no redirection characters. */
	else {
		cmd_len = strlen(str_redirect);
	}
	
	if (cmd_len == 0) {

		printf("myshell: syntax error: missing command\n");
		siglongjmp(sigint_env, 1);
	}

	if (ptr_in && in_len == 0) {

		printf("myshell: syntax error: missing input file\n");
		siglongjmp(sigint_env, 1);
	}

	if (ptr_out && out_len == 0) {

		printf("myshell: syntax error: missing output file\n");
		siglongjmp(sigint_env, 1);
	}
	

	/* Tokenize command string into args and store in struct. */
	cmd = malloc((size_t)cmd_len);
	strncpy(cmd, str_redirect, cmd_len);
	cmd[cmd_len] = '\0';
	command->args = split(cmd);


	/* Add input filename to command struct. */
	if (ptr_in) {
		
		command->file_in = malloc((size_t)in_len);
		strncpy(command->file_in, start_in, in_len);
		if(command->file_in[in_len-1] == ' ') command->file_in[in_len-1] = '\0'; //ensure that your file name does not contain spaces
		command->file_in[in_len] = '\0';
		command->file_in = trimWhitespace(command->file_in);
	}


	/* Add output filename to command struct. */
	if (ptr_out) {
		
		if (stdout_ctl == 1) {
			
			command->file_out = malloc((size_t)out_len);
			strncpy(command->file_out, start_out, out_len);
			if(command->file_out[out_len-1] == ' ') command->file_out[out_len-1] = '\0';
			command->file_out[out_len] = '\0';
			command->file_out = trimWhitespace(command->file_out);
		}

		else if (stdout_ctl == 2) {

			command->file_err = malloc((size_t)out_len);
			strncpy(command->file_err, start_out, out_len);
			if(command->file_err[out_len-1] == ' ') command->file_err[out_len-1] = '\0';
			command->file_err[out_len] = '\0';
			command->file_err = trimWhitespace(command->file_err);	
		}

		else if (stdout_ctl == 3) {

			command->file_out = malloc((size_t)out_len);
			command->file_err = malloc((size_t)out_len);
			strncpy(command->file_out, start_out, out_len);
			strncpy(command->file_err, start_out, out_len);
			if(command->file_out[out_len-1] == ' ') command->file_out[out_len-1] = '\0';
			if(command->file_err[out_len-1] == ' ') command->file_err[out_len-1] = '\0';
			command->file_out[out_len] = '\0';
			command->file_err[out_len] = '\0';
			command->file_out = trimWhitespace(command->file_out);
			command->file_err = trimWhitespace(command->file_err);
		}
	}
}


/**
* runCommands() iterates over the queue of command structs and executes each one according to the flags set for each during the parsing stage.
**/
void runCommands() {

	int i = 0;
	while(i < cmd_num) {

		if (*(cmd_buf[i].args[0])) {
			char *child_pathname = cmd_buf[i].args[0];
			char **child_tokens = cmd_buf[i].args;
			int in_fd = 0, out_fd = 0, err_fd = 0;


			/* Builtin commands don't get run via exec(). */
			if(builtinHelper(child_tokens)) return;


			pid_t pid = fork();


			/* Error. */
			if(pid < 0) {
				perror("Error");
				return;
			}


			/* Child calls dup2() for pipes/redirection if necessary, then exec's as either foreground or background. */
			else if (pid == 0) {


				/* Redirect stdin/stdout/stderr if necessary. */
				if (cmd_buf[i].pipe_fd_w > 0){
					if (dup2(cmd_buf[i].pipe_fd_w, 1) == -1) {
						perror("Error pipe_fd_w");
						exit(EXIT_FAILURE);
					}
				}

				if (cmd_buf[i].pipe_fd_r > 0){
					if (dup2(cmd_buf[i].pipe_fd_r, 0) == -1) {
						perror("Error pipe_fd_r");
						exit(EXIT_FAILURE);
					}
				}

				if(cmd_buf[i].file_in){
					if ((in_fd = open(cmd_buf[i].file_in,  O_RDONLY, 0444)) < 0){						
						perror(cmd_buf[i].file_in);	
						exit(EXIT_FAILURE);
					}
					if (dup2(in_fd, 0) == -1) {
						perror("Error in_fd");
						exit(EXIT_FAILURE);
					}
				}
				if(cmd_buf[i].file_out){
					if ((out_fd = open(cmd_buf[i].file_out,  O_CREAT|O_TRUNC|O_WRONLY, 0644)) < 0){
						perror(cmd_buf[i].file_out);	
						exit(EXIT_FAILURE);
					}
					if (dup2(out_fd, 1) == -1) {
						perror("Error out_fd");
						exit(EXIT_FAILURE);
					}
				}
				if(cmd_buf[i].file_err){
					if ((err_fd = open(cmd_buf[i].file_err,  O_CREAT|O_TRUNC|O_WRONLY, 0644)) < 0){
						perror(cmd_buf[i].file_err);	
						exit(EXIT_FAILURE);
					}
					if (dup2(err_fd, 2) == -1) {
						perror("Error err_fd");
						exit(EXIT_FAILURE);
					}
				}


				/* Background processes get put in a separate process group from myshell. */
				if (cmd_buf[i].pgid) {

					if (i == 0 || cmd_buf[i-1].pgid != cmd_buf[i].pgid) {

						setpgid(0, 0);
					}

					else {
						setpgid(0, pgid_curr);
					}
				}


				if (execvp(child_pathname, child_tokens) == -1){
					if (strstr(cmd_buf[i].args[0], "/")) printf("myshell: %s: No such file or directory\n", cmd_buf[i].args[0]);
					else printf("myshell: %s: command not found...\n", cmd_buf[i].args[0]);
					exit(EXIT_FAILURE);
				}
			}


			/* Parent closes pipe file descriptors so that the pipe can close and child processes can terminate.
			   If the child was run in the foreground, wait for it to terminate before continuing.
			   Otherwise, notify the user that the pipeline is running in the background and continue without waiting. */
			else {


				/* Close pipe file descriptors. */
				if (cmd_buf[i].pipe_fd_w)
					close(cmd_buf[i].pipe_fd_w);

				if (cmd_buf[i].pipe_fd_r)
					close(cmd_buf[i].pipe_fd_r);


				/* If command was run with &, update current process group id and continue. */
				if (cmd_buf[i].pgid) {

					if (i == 0 || cmd_buf[i-1].pgid != cmd_buf[i].pgid) {

						pgid_curr = cmd_buf[i].pgid;
						printf("[%d] %d\n", pgid_curr, pid);
					}

					else {
						//do nothing
					}
				}


				/* If the child was run in the foreground, wait for it to terminate. */
				else {
					pid_buf[pid_idx] = pid;
					pid_idx++;

					waitpid(pid, NULL, NULL);

					int j = 0;
					for(; j < pid_idx; j++){
						if (pid_buf[j] == pid){
							pid_buf[j] = 0;
							pid_idx--;
							break;
						}
						else if (pid_buf[j] == 0) j--;
					}
				}

				i++;
			}
		}
	}
}


/**
* cleanup() frees heap memory and resets global variables that track the command queue to a clean state for the next iteration of the myshell() loop.
**/
void cleanup() {

	int i;

	for (i = 0; i < cmd_num; i++) {

		if (cmd_buf[i].args) {
			free(cmd_buf[i].args);
			cmd_buf[i].args = NULL;
		}
			
		cmd_buf[i].pipe_fd_r = 0;
		cmd_buf[i].pipe_fd_w = 0;
		cmd_buf[i].file_in = NULL;
		cmd_buf[i].file_out = NULL;
		cmd_buf[i].file_err = NULL;
		cmd_buf[i].pgid = 0;
	}

	for (i = 0; i < sizeof(pid_buf) / sizeof(pid_t); i++) {
		pid_buf[i] = 0;
	}

	if (cmd_buf) {
		free(cmd_buf);
		cmd_buf = NULL;
	}

	cmd_num = 0;
	pid_idx = 0;
	pgid_counter = 0;
	pgid_curr = 0;
}


/**
* builtinHelper() handles select builtin shell commands that do not have a program file to run.
* Namely, we handle cd, help, echo, and exit.
**/
int builtinHelper(char** tokens){

	if(strcmp(*tokens, "cd") == 0){
		chdir(tokens[1]);
		return 1;
	}

	else if(strcmp(*tokens, "help") == 0){
		helpCMD();
		return 1;
	}

	else if(strcmp(*tokens, "echo") == 0){
		tokens++;
		while(*tokens){
			printf("%s ", *tokens);
			tokens++;
		}
		printf("\n");
		return 1;
	}

	if(strcmp(*tokens, "exit") == 0){
		exit(EXIT_SUCCESS);
	} 
	return 0; 
}


/**
* helpCMD() prints a README message to aid the user in running the myshell program.
**/
void helpCMD(){

	printf("\nWelcome to myshell! We support the following actions:\n\tBasic shell commands\n\tSeperating commands using ';'\n\tStandard input rediertion using '<'\n\tStandard output redirection using '>' or '1'>\n\tStandard error redirection using '2>'\n\tA combination of standard output and error using '&>'\n\tPipping using '|'\n\tRunning processes in the background by following a command using '&'\n\nYou may exit myshell by typing 'ctrl-d' or 'exit'\n\nTyping 'ctrl-c' will not exit myshell, it will quit a running program in myshell, and return back to the myshell prompt\n\n");
}

int main(int argc, char **argv) {

	/* Call cleanup() function on process exit. */
	atexit(cleanup);


	/* Handle ^C with handler(). */ 
	signal(SIGINT, handler);


	/* Call myshell() to start the main loop of the program. */
	myshell();


	return EXIT_SUCCESS;
}
