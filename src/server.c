/*
#  File:     server.c
#
#  Author:   David Rebatto
#  e-mail:   David.Rebatto@mi.infn.it
#
#
#  Revision history:
#   20 Mar 2004 - Original release.
#   23 Apr 2004 - Command parsing moved to a dedicated module.
#   25 Apr 2004 - Handling of Job arguments as list in classad
#                 Result buffer made persistent between sessions
#   29 Apr 2004 - Handling of Job arguments as list in classad removed
#                 Result buffer no longer persistant
#    7 May 2004 - 'Write' commands embedded in locks to avoid output corruption
#                 Uses dynamic strings to retrieve classad attributes' value
#   12 Jul 2004 - (prelz@mi.infn.it). Fixed quoting style for environment.
#   13 Jul 2004 - (prelz@mi.infn.it). Make sure an entire command is assembled 
#                                     before forwarding it.
#   20 Sep 2004 - Added support for Queue attribute.
#
#  Description:
#   Serve a connection to a blahp client, performing appropriate
#   job operations according to client requests.
#
#
#  Copyright (c) 2004 Istituto Nazionale di Fisica Nucleare (INFN).
#  All rights reserved.
#  See http://grid.infn.it/grid/license.html for license details.
#
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "blahpd.h"
#include "classad_c_helper.h"
#include "commands.h"
#include "job_status.h"
#include "resbuffer.h"
#include "mtsafe_popen.h"

#define COMMAND_PREFIX "-c"
#define PERSISTENT_BUFFER BUFFER_DONT_SAVE
#define JOBID_PREFIX            "BLAHP_JOBID_PREFIX"
#define JOBID_PREFIX_LEN        18
#define HOLD_JOB                1
#define RESUME_JOB              0
#define MAX_LRMS_NUMBER 	10
#define MAX_LRMS_NAME_SIZE	4
#define MAX_CERT_SIZE		100000
  
t_resline *first_job = NULL;
t_resline *last_job = NULL;
int num_jobs = 0;

#define NO_QUOTE     0
#define SINGLE_QUOTE 1
#define DOUBLE_QUOTE 2
#define INT_NOQUOTE  3

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif

const char *opt_format[4] = {
	" %s %s",               /* NO_QUOTE */
	" %s \"%s\"",           /* SINGLE_QUOTE */
	" %s \"\\\"%s\\\"\"",   /* DOUBLE_QUOTE */
	" %s %d"                /* INT_NOQUOTE */
};

char* statusstring[]={
 "IDLE",
 "RUNNING",
 "REMOVED",
 "IDLE",
 "HELD",
};

/* Function prototypes */
char *get_command(int client_socket);
char *escape_spaces(const char *str);
int set_cmd_list_option(char **command, classad_context cad, const char *attribute, const char *option);
int set_cmd_string_option(char **command, classad_context cad, const char *attribute, const char *option, const int quote_style);
int limit_proxy(char* proxyname);

/* Global variables */
static int server_socket;
static int async_mode = 0;
static int async_notice = 0;
static int exit_program = 0;
static pthread_mutex_t send_lock  = PTHREAD_MUTEX_INITIALIZER;
/* char *server_lrms; */
char *blah_script_location;
char *blah_version;
char lrmslist[MAX_LRMS_NUMBER][MAX_LRMS_NAME_SIZE];
int  lrms_counter = 0;
int  glexec_mode = 0;
char *bssp = NULL;
char *gloc = NULL;
/* Free all tokens of a command
 * */
void
free_args(char **arg_array)
{
	char **arg_ptr;
	
	if (arg_array)
	{
		for (arg_ptr = arg_array; (*arg_ptr) != NULL; arg_ptr++)
			free(*arg_ptr);
		free(arg_array);
	}
}	
/* Main server function 
 * */
int
serveConnection(int cli_socket, char* cli_ip_addr)
{
	char *input_buffer;
	char *reply;
	char *result;
	fd_set readfs;
	int exitcode = 0;
	int reply_len;
	pthread_t task_tid;
	int i, argc;
	char **argv;
	command_t *command;
	FILE *conffile = NULL;
	char *conffilestr = NULL;
	char buffer[128];
	int bc=0;

	init_resbuffer();
	if (cli_socket == 0) server_socket = 1;
	else                 server_socket = cli_socket;

	/* Get values from environment */
	if ((result = getenv("GLITE_LOCATION")) == NULL)
	{
		result = DEFAULT_GLITE_LOCATION;
	}
	blah_script_location = make_message(BINDIR_LOCATION, result);
/*	if ((server_lrms = getenv("BLAH_LRMS")) == NULL)
	{
		server_lrms = DEFAULT_LRMS;
	} */
	blah_version = make_message(RCSID_VERSION, VERSION, "poly");
        if ((gloc=getenv("GLEXEC_COMMAND")) == NULL)
        {
        	gloc = DEFAULT_GLEXEC_COMMAND;
        }
	conffilestr = make_message("%s/etc/blah.config",result);
	if((conffile = fopen(conffilestr,"r")) != NULL)
	{
		while(fgets(buffer, 128, conffile))
		{
			if (!strncmp (buffer,"supported_lrms=", strlen("supported_lrms=")))
			{
				bc+=strlen("supported_lrms=");
                                while(strlen(&buffer[bc]) > 0)
                                {
                                        strncpy(lrmslist[lrms_counter],&buffer[bc],3);
                                        lrms_counter++;
					if(strlen(&buffer[bc]) > 3) bc+=4;
					else 
					   break;
                                }
			}
			bc=0;
			memset(buffer,0,128);
		}
		
		fclose(conffile);
		free(conffilestr);
	}
	
	write(server_socket, blah_version, strlen(blah_version));
	write(server_socket, "\r\n", 2);
	while(!exit_program)
	{
		input_buffer = get_command(cli_socket);
		if (input_buffer)
		{
			if (parse_command(input_buffer, &argc, &argv) == 0)
				command = find_command(argv[0]);
			else
				command = NULL;

			if (command)
			{
				if (argc != (command->required_params + 1))
				{
					reply = make_message("E expected\\ %d\\ parameters,\\ %d\\ found\\r\n", command->required_params, argc -1);
				}
				else
				{
					if (command->threaded)
					{
						if(pthread_create(&task_tid, NULL, command->cmd_handler, (void *)argv))
						{
							reply = make_message("F Cannot\\ start\\ thread\r\n");
						}
						else
						{
							reply = make_message("S\r\n");
							pthread_detach(task_tid);
						}
						/* free argv in threaded function */
					}
					else
					{
						if ((result = (char *)command->cmd_handler(argv)) == NULL)
						{
							reply = make_message("F\r\n");
						}
						else
						{
							reply = make_message("S %s\r\n", result);
							free(result);
						}
						free_args(argv);
					}
				}
			}
			else
			{
				reply = make_message("E Unknown\\ command\r\n");
				free_args(argv);
			}

			pthread_mutex_lock(&send_lock);
			if (reply)
			{
				if (write(server_socket, reply, strlen(reply)) > 0)
					remove(FLUSHED_BUFFER);
				free(reply);
			}
			else
				write(server_socket, "E Cannot\\ allocate\\ return\\ line\r\n", 34);
			pthread_mutex_unlock(&send_lock);
			
			free(input_buffer);
		}
		else /* command was NULL */
		{
			fprintf(stderr, "Connection closed by remote host\n");
			exitcode = 1;
			break;
		}

	}
	if (cli_socket != 0) 
	{
		shutdown(cli_socket, SHUT_RDWR);
		close(cli_socket);
	}

	free(blah_script_location);
	free(blah_version);

	exit(exitcode);
}

/* Non threaded commands
 * --------
 * must return a pointer to a free-able string or NULL
 * */

void *
cmd_quit(void *args)
{
	char *result = NULL;
	
	exit_program = 1;
	result = strdup("Server\\ exiting");
	return(result);
}

void *
cmd_commands(void *args)
{
	char *result;

	result = known_commands();
	return(result);
}

void *
cmd_version(void *args)
{
	char *result;

	result = strdup(blah_version);
	return(result);	
}

void *
cmd_async_on(void *args)
{
	char *result;

	async_mode = async_notice = 1;
	result = strdup("Async\\ mode\\ on");
	return(result);
}
			
void *
cmd_async_off(void *args)
{
	char *result;

	async_mode = async_notice = 0;
	result = strdup("Async\\ mode\\ off");
	return(result);
}

void *
cmd_results(void *args)
{
	char *result;
	char *res_lines;
	char *tmp_realloc;

	if (result = (char *) malloc (13)) /* hope 10 digits suffice*/
	{
		snprintf(result, 10, "%d", num_results());
		if(num_results())
		{
			strcat(result, "\r\n");
			res_lines = get_lines(BUFFER_FLUSH);
			if ((tmp_realloc = (char *)realloc(result, strlen(result) + strlen(res_lines) + 2)) == NULL)
			{
				free(result);
				free(res_lines);
				return(NULL);
			}
			result = tmp_realloc;
			strcat(result, res_lines);
			free(res_lines);
		}

		/* From now on, send 'R' when a new resline is enqueued */
		async_notice = async_mode;
	}
	
	/* If malloc has failed, return NULL to notify error */
	return(result);
}

void *
cmd_set_glexec_dn(void *args)
{

        char *result  = NULL;
        struct stat buf;
        char **argv = (char **)args;
	char *proxt4= argv[1];
        char *ssl_client_cert = argv[2];
	FILE *dummy;
        char certbuffer[MAX_CERT_SIZE];
	int res = 0;
	char *cmdstr = NULL;
	char *proxynameNew = NULL;
	
	if((!stat(proxt4, &buf))&&(!stat(ssl_client_cert, &buf)))
        {
                res = setenv("GLEXEC_MODE","lcmaps_get_account",1);
                if (res)
                {
                        result=strdup("Unable\\ to\\ set\\ GLEXEC\\ MODE");
                        return(result);
                }
		res = setenv("BLAHP_SAVE_SOURCE_PROXY",proxt4,1);
                if (res)
                {
			result=strdup("Unable\\ to\\ set\\ GLEXEC\\ MODE");
                        return(result);
                }
		dummy = fopen(ssl_client_cert, "r");
		if (dummy == NULL)
		{ 
			unsetenv("BLAHP_SAVE_SOURCE_PROXY");
			result=strdup("Unable\\ to\\ read\\ SSL\\ CLIENT\\ CERT");
			return(result);
		}

		/* reads from cert file and stores in certbuffer */
		fread(certbuffer, MAX_CERT_SIZE, 1, dummy);
		fclose(dummy);
		if(strlen(certbuffer) > 0)
		{
			/* save blah_save_source_proxy */
			bssp = strdup(proxt4);			
			res = setenv("SSL_CLIENT_CERT",certbuffer,1);
			/* proxt4 must be limited for subsequent submission */		
               		proxynameNew=make_message("%s.lmt",proxt4);
               		cmdstr=make_message("cp %s %s",proxt4, proxynameNew);
               		result=system(cmdstr);
                        if(result)
                        {
                        	result = make_message("Error\\ reading\\ proxy\\ %s", proxt4);
                        	free(proxynameNew);
                        	free(cmdstr);
                        	unsetenv("SSL_CLIENT_CERT");
				return(result);
                        }               		


			limit_proxy(proxynameNew);
               		bssp = strdup(proxynameNew);
			free(cmdstr);
			free(proxynameNew);
		}
		else
                {
                        result=strdup("Unable\\ to\\ read\\ SSL\\ CLIENT\\ CERT");
                	glexec_mode = 0;                        
			return(result);
                }
		glexec_mode = 1;
                result = strdup("Glexec\\ mode\\ on");
	}else
                result = strdup("Proxy\\ file\\ not\\ found");

	return(result);
}

void *
cmd_unset_glexec_dn(void *args)
{
        char *result;

        unsetenv("GLEXEC_MODE");
        unsetenv("BLAHP_SAVE_SOURCE_PROXY");
        unsetenv("SSL_CLIENT_CERT");
	glexec_mode = 0;
        if(bssp!=NULL)
	{
		free(bssp);
		bssp = NULL;
	}
	result=make_message("Glexec\\ mode\\ off");
        return (result);
}

/* Threaded commands
 * N.B.: functions must free argv before return
 * */

void *
cmd_submit_job(void *args)
{
	const char *submission_cmd;
	FILE *cmd_out;
	int retcod;
	char *command;
	char jobId[JOBID_MAX_LEN];
	char *resultLine;
	char **argv = (char **)args;
	char **arg_ptr;
	classad_context cad;
	char *reqId = argv[1];
	char *jobDescr = argv[2];
	char *server_lrms = NULL;
	int result;
	char error_message[ERROR_MAX_LEN];
	char *error_string;
	int res = 1;
	char* resfg = NULL;
	char *proxyname   = NULL;
        char *proxynameNew   = NULL;
	char *cmdstr = NULL;
	char *command_ext = NULL;
	cad = classad_parse(jobDescr);
	if (cad == NULL)
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 1 Error\\ parsing\\ classad N/A", reqId);
		goto cleanup_argv;
	}


	/* Get the lrms type from classad attribute "gridtype" */
	result = classad_get_dstring_attribute(cad, "gridtype", &server_lrms);
	if (result != C_CLASSAD_NO_ERROR)
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 1 Missing\\ gridtype\\ in\\ submission\\ classAd N/A", reqId);
		goto cleanup_cad;
	}

	if(glexec_mode)
	{
		setenv("GLEXEC_SOURCE_PROXY",bssp,1);
		result = classad_get_dstring_attribute(cad, "x509UserProxy", &proxyname);
                if (result == C_CLASSAD_NO_ERROR)
		{
			setenv("GLEXEC_TARGET_PROXY",proxyname,1);
		}else{
                       /* PUSH A FAILURE */
                       resultLine = make_message("%s 1 Out\\ of\\ memory\\ parsing\\ classad N/A", reqId);
                       free(server_lrms);
                       unsetenv("GLEXEC_SOURCE_PROXY");
                       unsetenv("GLEXEC_TARGET_PROXY");
                       enqueue_result(resultLine);
                       return;        
	}
		command = make_message("%s %s/%s_submit.sh -x %s", gloc, blah_script_location, server_lrms, proxyname);
	}else
		command = make_message("%s/%s_submit.sh", blah_script_location, server_lrms);


	if (command == NULL)
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 1 Out\\ of\\ Memory N/A", reqId);
		goto cleanup_lrms;
	}

        /* Get the proxy name from classad attribute "X509UserProxy" */
        if (((result = classad_get_dstring_attribute(cad, "x509UserProxy", &proxyname)) == C_CLASSAD_NO_ERROR)
	   &&(!glexec_mode ))
        { 
               proxynameNew=make_message("%s.lmt",proxyname);
               cmdstr=make_message("cp %s %s",proxyname, proxynameNew);
               system(cmdstr);
               limit_proxy(proxynameNew);
	       command_ext = make_message("%s -x %s", command, proxynameNew);
               free(proxyname);
               free(cmdstr);
               if (command_ext == NULL)
               {
                       /* PUSH A FAILURE */
                       resultLine = make_message("%s 1 Out\\ of\\ memory\\ parsing\\ classad N/A", reqId);
                       free(proxynameNew);
                       goto cleanup_command;
               }

               /* Swap new command in */
               free(command);
               command = command_ext;
               free(proxynameNew);
	}

	/* Cmd attribute is mandatory: stop on any error */
	if (set_cmd_string_option(&command, cad, "Cmd", COMMAND_PREFIX, NO_QUOTE) != C_CLASSAD_NO_ERROR)
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 7 Cannot\\ parse\\ Cmd\\ attribute\\ in\\ classad N/A", reqId);
		goto cleanup_command;
	}
	
	/* All other attributes are optional: fail only on memory error 
	   IMPORTANT: Args must alway be the last!
	*/
	if ((set_cmd_string_option(&command, cad, "In",         "-i", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_string_option(&command, cad, "Out",        "-o", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_string_option(&command, cad, "Err",        "-e", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_string_option(&command, cad, "Iwd",        "-w", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_string_option(&command, cad, "Env",        "-v", SINGLE_QUOTE)  == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_string_option(&command, cad, "Queue",      "-q", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_int_option   (&command, cad, "NodeNumber", "-n", INT_NOQUOTE)   == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_bool_option  (&command, cad, "StageCmd",   "-s", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_string_option(&command, cad, "ClientJobId","-j", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY) ||
	    (set_cmd_string_option(&command, cad, "Args",      	"--", NO_QUOTE)      == C_CLASSAD_OUT_OF_MEMORY))
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 1 Out\\ of\\ memory\\ parsing\\ classad N/A", reqId);
		goto cleanup_command;
	}

	/* Execute the submission command */
	/* fprintf(stderr, "DEBUG: submission cmd = '%s'\n", command); */
	if((cmd_out = mtsafe_popen(command, "r")) == NULL)
	{
		/* PUSH A FAILURE */
		/* errno is not set if popen fails because of low memory */
		if (retcod = errno)
			strerror_r(errno, error_message, sizeof(error_message));
		else
			strncpy(error_message, "Cannot open pipe for the command: out of memory", sizeof(error_message));

		error_string = escape_spaces(error_message);
		resultLine = make_message("%s %d %s", reqId, retcod, error_string);
		free(error_string);
		goto cleanup_command;
	}
	
	while(1)
	{
	 	resfg = fgets(jobId, sizeof(jobId), cmd_out);
		if (resfg == NULL) break;
		res = strncmp(jobId,JOBID_PREFIX,JOBID_PREFIX_LEN);
		if (res == 0) break;
	}

	retcod = mtsafe_pclose(cmd_out);
	if ((retcod != 0)||(res != 0))
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 2 Submit\\ command\\ didn't\\ return\\ jobId\\ (exit code = %d) N/A", reqId, WEXITSTATUS(retcod));
		goto cleanup_command;
	}
	
	if (jobId[strlen(jobId) - 1] == '\n') jobId[strlen(jobId) - 1] = '\000';
 	
	/* PUSH A SUCCESS */
	resultLine = make_message("%s 0 No\\ error %s", reqId, jobId + JOBID_PREFIX_LEN);

	/* Free up all arguments and exit (exit point in case of error is the label
           pointing to last successfully allocated variable) */
cleanup_command:
	free(command);
cleanup_lrms:
	free(server_lrms);
cleanup_cad:
	classad_free(cad);
cleanup_argv:
	free_args(argv);
	enqueue_result(resultLine);
	free(resultLine);
	if(glexec_mode)
	{
		unsetenv("GLEXEC_SOURCE_PROXY");
        	unsetenv("GLEXEC_TARGET_PROXY");
	}
	return;
}

void *
cmd_cancel_job(void* args)
{
	int retcod;
	FILE *dummy;
	char *command;
	char *resultLine = NULL;
	char **argv = (char **)args;
	char **arg_ptr;
	char *server_lrms;
	char *reqId = argv[1];
	char *jobId;
	char error_message[1024];
	char *error_string;

	/* The job Id needs at least 4 chars for the "<lrms>/" prefix */
	if (strlen(argv[2]) < 4)
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 2 Malformed\\ jobId %s", reqId, jobId);
		goto cleanup_argv;
	}

	/* Split <lrms> and actual job Id */
	if((server_lrms = strdup(argv[2])) == NULL)
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 1 Cannot\\ allocate\\ memory\\ for\\ the\\ lrms\\ string", reqId);
		goto cleanup_argv;
	}
	server_lrms[3] = '\0';
	jobId = server_lrms + 4;

	/* Prepare the cancellation command */
	if(glexec_mode )
	{
        	command = make_message("%s %s/%s_cancel.sh %s", gloc, blah_script_location, server_lrms, jobId);
	}else
	{
		command = make_message("%s/%s_cancel.sh %s", blah_script_location, server_lrms, jobId);
	}
	if (command == NULL)
	{
		/* PUSH A FAILURE */
		resultLine = make_message("%s 1 Cannot\\ allocate\\ memory\\ for\\ the\\ command\\ string", reqId);
		goto cleanup_lrms;
	}

	/* Execute the command */
	/* fprintf(stderr, "DEBUG: executing %s\n", command); */
	if((dummy = mtsafe_popen(command, "r")) == NULL)
	{
		/* PUSH A FAILURE */
		/* errno is not set if popen fails because of low memory */
		if (retcod = errno)
			strerror_r(errno, error_message, sizeof(error_message));
		else
			strncpy(error_message, "Cannot open pipe for the command: out of memory", sizeof(error_message));

		error_string = escape_spaces(error_message);
		resultLine = make_message("%s %d %s", reqId, retcod, error_string);
		free(error_string);
		goto cleanup_command;
	}	
	retcod = mtsafe_pclose(dummy);
	resultLine = make_message("%s %d %s", reqId, retcod, retcod ? "Error" : "No\\ error");

	/* Free up all arguments and exit (exit point in case of error is the label
           pointing to last successfully allocated variable) */
cleanup_command:
	free(command);
cleanup_lrms:
	free(server_lrms);
cleanup_argv:
	free_args(argv);
	enqueue_result(resultLine);
	free (resultLine);

	return;
}

void*
cmd_status_job(void *args)
{
        classad_context status_ad[MAX_JOB_NUMBER];
        char *str_cad;
        char *esc_str_cad;
        char *resultLine;
        char **argv = (char **)args;
        char **arg_ptr;
        char errstr[MAX_JOB_NUMBER][ERROR_MAX_LEN];
        char *esc_errstr;
        char *reqId = argv[1];
        char *jobDescr = argv[2];
        int jobStatus, retcode;
        int i, job_number;

        retcode = get_status(jobDescr, status_ad, errstr, 0, &job_number);
        if (!retcode)
        {
                for(i = 0;i < job_number; i++)
                {
                        esc_errstr = escape_spaces(errstr[i]);
                        if(status_ad[i] != NULL )
                        {
                                        classad_get_int_attribute(status_ad[i], "JobStatus", &jobStatus);
                                        str_cad = classad_unparse(status_ad[i]);
                                        esc_str_cad = escape_spaces(str_cad);
                                        if(job_number > 1)
                                                resultLine = make_message("%s.%d %d  %d %s", reqId, i,retcode, jobStatus, esc_str_cad);
                                        else
                                                resultLine = make_message("%s %d  %d %s", reqId, retcode, jobStatus, esc_str_cad);
                                        classad_free(status_ad[i]);
                                        free(str_cad);
                                        free(esc_str_cad);
                        }else
                        {
                                        if(job_number > 1)
                                                resultLine = make_message("%s.%d 1 %s", reqId, i, esc_errstr);
                                        else
                                                resultLine = make_message("%s 1 %s" ,reqId, esc_errstr);
                        }
                        free(esc_errstr);
                        enqueue_result(resultLine);
                        free(resultLine);
                }
        }
        else
        {
                resultLine = make_message("%s %d %s N/A", reqId, retcode, errstr[i]);
                enqueue_result(resultLine);
                free(resultLine);
        }
        /* Free up all arguments */
        free_args(argv);
        return;
}


void *
cmd_renew_proxy(void *args)
{
	classad_context status_ad[MAX_JOB_NUMBER];
	char *resultLine;
	char **argv = (char **)args;
	char **arg_ptr;
	char errstr[MAX_JOB_NUMBER][ERROR_MAX_LEN];
	char *esc_errstr;
	char *reqId = argv[1];
	char *jobDescr = argv[2];
	char *proxyFileName = argv[3];
	char *workernode;
	char *command;
	char *server_lrms;
	char *esc_strerr;
	char *proxy_link;
	char old_proxy[FILENAME_MAX];
	int jobStatus, retcod, result;
	FILE *dummy;
	char error_message[ERROR_MAX_LEN];
	char *error_string;
	char *proxyFileNameNew;
	int  job_number;

	retcod = get_status(jobDescr, status_ad, errstr, 1, &job_number);
	if(!glexec_mode)
	{
		proxyFileNameNew = make_message("%s.lmt",proxyFileName);
	}else
		proxyFileNameNew = strdup(proxyFileName);
	if (!strcmp(errstr[0],"No Error"))
	{
			esc_errstr = escape_spaces(errstr[0]);
			classad_get_int_attribute(status_ad[0], "JobStatus", &jobStatus);
			jobDescr = strrchr(jobDescr, '/') + 1;
			switch(jobStatus)
			{
				case 1: /* job queued */
					/* copy the proxy locally */
					proxy_link = make_message("%s/.blah_jobproxy_dir/%s.proxy", getenv("HOME"), jobDescr);
					if ((result = readlink(proxy_link, old_proxy, sizeof(old_proxy) - 1)) == -1)
					{
						esc_strerr = escape_spaces(strerror(errno));
						resultLine = make_message("%s 1 Error\\ locating\\ original\\ proxy: %s", reqId, esc_strerr);
						free(esc_strerr);
					}
					else
					{
						old_proxy[result] = '\0'; /* readlink doesn't append the NULL char */
						if (strcmp(proxyFileNameNew, old_proxy) != 0) /* If Condor didn't change the old proxy file already */
						{
							
                                                        if(!glexec_mode)
                                                        {
								//if (rename(proxyFileName, old_proxy) == 0) /* FIXME with a safe portable rotation */
								/* proxy must be copied and (if we are not in glexec_mode) limited */
        							command = make_message("cp -f %s %s &>2 /dev/null",proxyFileName, old_proxy);
								if((dummy = mtsafe_popen(command, "r")) == NULL)
								{
                                                                	resultLine = make_message("%s 1 Error\\ reading\\ proxy\\ %s", reqId, proxyFileName);
									free(proxyFileNameNew);
                                                                	free(command);
									command=NULL;
									break;
								}	
								limit_proxy(proxyFileNameNew);
							}
							free(proxyFileNameNew);proxyFileNameNew=NULL;
							if(command) free(command);
							resultLine = make_message("%s 0 Proxy\\ renewed", reqId);
						}
						else
						{
							resultLine = make_message("%s 0 Proxy\\ renewed\\ (in\\ place\\ -\\ job\\ pending)", reqId);
						}
					}
					free (proxy_link);
					break;
				case 2: /* job running */
					/* send the proxy to remote host */
					if (((result = classad_get_dstring_attribute(status_ad[0], "WorkerNode", &workernode)) == C_CLASSAD_NO_ERROR)&&(strcmp(workernode,"")))
					{
						
                                                if(!glexec_mode)
                                                {
							/* proxy must be limited */
        						command = make_message("cp %s %s &>2 /dev/null",proxyFileName, proxyFileNameNew);
							if((dummy = mtsafe_popen(command, "r")) == NULL)
                                                	{
                                                		resultLine = make_message("%s 1 Error\\ reading\\ proxy\\ %s", reqId, proxyFileName);
                                                		free(proxyFileNameNew);
                                                		free(command);
								command=NULL;
                                                		break;
                                                	}else
                                                        mtsafe_pclose(dummy);
							limit_proxy(proxyFileNameNew);
						}
						
						if(command) { free(command); command=NULL;}
						command = make_message("export LD_LIBRARY_PATH=%s/lib; %s/BPRclient %s %s %s &>2 /dev/null",
				                        getenv("GLOBUS_LOCATION") ? getenv("GLOBUS_LOCATION") : "/opt/globus",
				                        blah_script_location, proxyFileNameNew, jobDescr, workernode);
						free(workernode);
						workernode=NULL;
						free(proxyFileNameNew);proxyFileNameNew=NULL;
						/* Execute the command */
						/* fprintf(stderr, "DEBUG: executing %s\n", command); */
						if((dummy = mtsafe_popen(command, "r")) == NULL)
						{
							/* PUSH A FAILURE */
							/* errno is not set if popen fails because of low memory */
							if (retcod = errno)
								strerror_r(errno, error_message, sizeof(error_message));
							else
								strncpy(error_message, "Cannot open pipe for the command: out of memory", sizeof(error_message));

							error_string = escape_spaces(error_message);
							resultLine = make_message("%s %d %s", reqId, retcod, error_string);
						}	
						else
						{
							retcod = mtsafe_pclose(dummy);
							resultLine = make_message("%s %d %s", reqId, retcod, retcod ? "Error" : "No\\ error");
						}
						if(command) { free(command); command=NULL;}
					}else
					{
						resultLine = make_message("%s 1 Cannot\\ retrieve\\ executing\\ host", reqId);
					}
				break;
				case 3: /* job deleted */
					/* no need to refresh the proxy */
					resultLine = make_message("%s 0 No\\ proxy\\ to\\ renew\\ -\\ Job\\ was\\ deleted", reqId);
				break;
				case 4: /* job completed */
					/* no need to refresh the proxy */
					resultLine = make_message("%s 0 No\\ proxy\\ to\\ renew\\ -\\ Job\\ completed", reqId);
				break;
				case 5: /* job hold */
					/* FIXME not yet supported */
					resultLine = make_message("%s 0 No\\ support\\ for\\ renewing\\ held\\ jobs\\ yet", reqId);
				break;
				default:
					resultLine = make_message("%s 1 Wrong\\ state\\ (%d)", reqId, jobStatus);
			}
		}else
		{
			//resultLine = make_message("%s %d %s", reqId, retcod, esc_errstr);
			resultLine = make_message("%s %d %s", reqId, 1, escape_spaces(errstr[0]));
		}
		if (resultLine)
		{
			enqueue_result(resultLine);
			free(resultLine);
		}
		else
		{
			enqueue_result("Missing result line due to memory error");
			free(esc_errstr);
		}
	
	/* Free up all arguments */
	classad_free(status_ad[0]);
	free_args(argv);
	if(proxyFileNameNew) free(proxyFileNameNew);
	if(command) free(command);
	return;
}

void
hold_res_exec(char* jobdescr, char* reqId,char* action,int status )
{
        int retcod;
        FILE *dummy;
        char *command;
        char *resultLine = NULL;
        char *server_lrms;
        char *jobId;
        char error_message[1024];
        char *error_string;

        /* The job Id needs at least 4 chars for the "<lrms>/" prefix */
        if (strlen(jobdescr) < 4)
        {
                /* PUSH A FAILURE */
                resultLine = make_message("%s 2 Malformed\\ jobId %s", reqId, jobId);
                goto cleanup_argv;
        }

        /* Split <lrms> and actual job Id */
        if((server_lrms = strdup(jobdescr)) == NULL)
        {
                /* PUSH A FAILURE */
                resultLine = make_message("%s 1 Cannot\\ allocate\\ memory\\ for\\ the\\ lrms\\ string", reqId);
                goto cleanup_argv;
        }
        server_lrms[3] = '\0';
        jobId = server_lrms + 4;

        if(glexec_mode )
        {
                if(!strcmp(action,"hold"))
                {
                        command = make_message("%s %s/%s_%s.sh %s %d", gloc, blah_script_location, server_lrms, action, jobId, status);
                }else
                {
                        command = make_message("%s %s/%s_%s.sh %s", gloc, blah_script_location, server_lrms, action, jobId);
                }
        }else
        {
                if(!strcmp(action,"hold"))
                {
                        command = make_message("%s/%s_%s.sh %s %d", blah_script_location, server_lrms, action, jobId, status);
                }else
                {
                        command = make_message("%s/%s_%s.sh %s", blah_script_location, server_lrms, action, jobId);
                }
        }

        if (command == NULL)
        {
                /* PUSH A FAILURE */
                resultLine = make_message("%s 1 Cannot\\ allocate\\ memory\\ for\\ the\\ command\\ string", reqId);
                goto cleanup_lrms;
        }

        /* Execute the command */
        /* fprintf(stderr, "DEBUG: executing %s\n", command); */
        if((dummy = mtsafe_popen(command, "r")) == NULL)
        {
                /* PUSH A FAILURE */
                /* errno is not set if popen fails because of low memory */
                if (retcod = errno)
                        strerror_r(errno, error_message, sizeof(error_message));
                else
                        strncpy(error_message, "Cannot open pipe for the command: out of memory", sizeof(error_message));

                error_string = escape_spaces(error_message);
                resultLine = make_message("%s %d %s", reqId, retcod, error_string);
                free(error_string);
                goto cleanup_command;
        }
        retcod = mtsafe_pclose(dummy);
        if(retcod)
        {
                resultLine = make_message("%s %d Job\\ %s:\\ %s\\ not\\ supported\\ by\\ %s", reqId, retcod, statusstring[status - 1] ,action ,server_lrms );
        }else
        resultLine = make_message("%s %d No\\ error", reqId, retcod);
        /* Free up all arguments and exit (exit point in case of error is the label
           pointing to last successfully allocated variable) */
cleanup_command:
        free(command);
cleanup_lrms:
        free(server_lrms);
cleanup_argv:
        enqueue_result(resultLine);
        free (resultLine);

        return;
}

void
hold_resume(void* args, int action )
{
        classad_context status_ad[MAX_JOB_NUMBER];
        char **argv = (char **)args;
        char errstr[MAX_JOB_NUMBER][ERROR_MAX_LEN];
        char *resultLine = NULL;
        int jobStatus, retcode;
        char *reqId = NULL;
        char jobdescr[MAX_JOB_NUMBER][JOBID_MAX_LEN];
        int i,job_number;
        char *dummyargv = strdup(argv[2]);
        char *tmpjobdescr=NULL;

        /* job status check */
        retcode = get_status(dummyargv, status_ad, errstr, 0, &job_number);
        /* if multiple jobs are present their id must be extracted from argv[2] */
        i=0;
        tmpjobdescr = strtok(dummyargv," ");
        memcpy(jobdescr[0],tmpjobdescr,strlen(tmpjobdescr));
        if(job_number>1)
        {
                for (i =1; i < job_number;i++)
                {
                        tmpjobdescr= strtok(NULL," ");
                        memcpy(jobdescr[i],tmpjobdescr,strlen(tmpjobdescr));
                }
        }

        if (!retcode)
        {
                for (i=0;i<job_number;i++)
                {
                        if(job_number>1)
                        {
                                reqId= make_message("%s.%d",argv[1],i);
                        }else
				reqId= strdup(argv[1]);
                        if(classad_get_int_attribute(status_ad[i], "JobStatus", &jobStatus)==C_CLASSAD_NO_ERROR)
                        {
                                switch(jobStatus)
                                {
                                        case 1:/* IDLE */
                                                if(action == HOLD_JOB)
                                                {
                                                        hold_res_exec(jobdescr[i],reqId,"hold",1);
                                                }
                                                else
                                                if (resultLine = make_message("%s 1 Job\\ Idle\\ jobId %s", reqId,jobdescr[i]))
                                                {
                                                        enqueue_result(resultLine);
                                                        free(resultLine);
                                                }
                                        break;
                                        case 2:/* RUNNING */
                                                if(action == HOLD_JOB)
                                                {
                                                        hold_res_exec(jobdescr[i],reqId,"hold",2);
                                                }else
                                                if (resultLine = make_message("%s 1 \\ Job\\ Running\\ jobId %s", reqId, jobdescr[i]))
                                                {
                                                        enqueue_result(resultLine);
                                                        free(resultLine);
                                                }
                                        break;
                                        case 3:/* REMOVED */
                                                if (resultLine = make_message("%s 1 Job\\ Removed\\ jobId %s", reqId, jobdescr[i]))
                                                {
                                                        enqueue_result(resultLine);
                                                        free(resultLine);
                                                }
                                        break;
                                        case 4:/* COMPLETED */
                                                if (resultLine = make_message("%s 1 Job\\ Completed\\ jobId %s", reqId, jobdescr[i]))
                                                {
                                                        enqueue_result(resultLine);
                                                        free(resultLine);
                                                }
                                        break;
                                        case 5:/* HELD */
                                                if(action == RESUME_JOB)
                                                        hold_res_exec(jobdescr[i],reqId,"resume",5);
                                                else
                                                if (resultLine = make_message("%s 0 Job\\ Held\\ jobId %s", reqId, jobdescr[i]))
                                                {
                                                        enqueue_result(resultLine);
                                                        free(resultLine);
                                                }
                                        break;
                                }
                        }else
                        if (resultLine = make_message("%s 1 %s", reqId, errstr[i]))
                        {
                                enqueue_result(resultLine);
                                free(resultLine);
                        }
			if(reqId) {free(reqId);reqId=NULL;}
                }
        }else
        {
                resultLine = make_message("%s %d %s", reqId, retcode, errstr[0]);
                enqueue_result(resultLine);
                free(resultLine);
        }
        if (dummyargv) free(dummyargv);
        if (reqId) free(reqId);
        return;
}







void *
cmd_hold_job(void* args)
{
	hold_resume(args,HOLD_JOB);
	return;
}

void *
cmd_resume_job(void* args)
{
        hold_resume(args,RESUME_JOB);
        return;
}

void *
cmd_get_hostport(void *args)
{
        char **argv = (char **)args;
        char *reqId = argv[1];
        FILE *cmd_out;
        char *command;
        char *resultLine;
        char hostport[1024];
        int  retcode;
	int i;
	
	if(lrms_counter)
	{
		resultLine = make_message("%s 0 ", reqId);
		for(i =0;i < lrms_counter;i++)
		{        
			//command = make_message("%s/%s_status.sh -n", blah_script_location, server_lrms);
        		command = make_message("%s/%s_status.sh -n", blah_script_location, lrmslist[i]);
			if ((cmd_out=mtsafe_popen(command, "r")) == NULL)
        		{
                		resultLine = make_message("%s 1 Unable\\ to\\ execute\\ %s", reqId,  command );
                		enqueue_result(resultLine);
                		free(command);
                		free_args(argv);
                		return;
        		}
			fgets(hostport, sizeof(hostport), cmd_out);
			if (hostport[strlen(hostport) - 1] == '\n') hostport[strlen(hostport) - 1] = '\0';
        		retcode = mtsafe_pclose(cmd_out);
        		if((!retcode)&&(strlen(hostport) != 0)) 
        			resultLine = make_message("%s%s/%s\ ", resultLine,lrmslist[i], hostport);			
			else
				resultLine = make_message("%s%s/%s\ ", resultLine, lrmslist[i], "Error\\ reading\\ host:port");		
        		free(command);
		}
		enqueue_result(resultLine);
		free(resultLine);
        }

	free_args(argv);
        return ;
}

/* Utility functions
 * */

char 
*escape_spaces(const char *str)
{
	char *buffer;
	int i, j;

	if ((buffer = (char *) malloc (strlen(str) * 2 + 1)) == NULL)
	{
		fprintf(stderr, "Out of memory.\n");
		exit(MALLOC_ERROR);
	}

	for (i = 0, j = 0; i <= strlen(str); i++, j++)
	{
		if (str[i] == ' ') buffer[j++] = '\\';
		buffer[j] = str[i];
	}
	realloc(buffer, strlen(buffer) + 1);
	return(buffer);
}

char*
get_command(int s)
{
	static char *cmd_queue = NULL;
	static char *next_cmd;
	static char *queue_end;
	char *message = NULL;
	char *tmp_realloc;
	int allocated_size = 0;
	char buffer[2047];
	int read_chars = 0; 
	int recv_chars, i;
	int done = FALSE;
	char last_char;

	/* if the queue is empty, read from the socket */
	if (!cmd_queue)
	{
		while (!done)
		{
			if ((recv_chars = read(s, buffer, sizeof(buffer))) > 0)
			{
				if ((read_chars + recv_chars) > allocated_size)
				{
					allocated_size += sizeof(buffer) + 1;
					tmp_realloc = (char *) realloc (message, allocated_size);
					if (tmp_realloc == NULL)
					{
						allocated_size = 0;
						perror("Error allocating buffer for incoming message");
						close(s);
						if (message) free(message);
						exit(MALLOC_ERROR);
					}
					else
						message = tmp_realloc;
				}
				memcpy(&message[read_chars], buffer, recv_chars);
				read_chars += recv_chars;
				message[read_chars] = '\000';
			} else {
				/* Error or EOF */
				break;
			}
			if (message != NULL) {
				/* Require LF terminated messages */
				last_char = message[read_chars -1];
				if (last_char == '\n') break;
			}
		}
	
		if (recv_chars <= 0)
		{
			return(NULL);
		}
		else if (read_chars > 0)
		{
			/* return(message); */
			cmd_queue = strdup(message);
			next_cmd = cmd_queue;
			queue_end = cmd_queue + read_chars;
			free(message);
		}
	}

	/* save the pointer to current command */
	message = next_cmd;
	
	/* search for end of current command */
	while(next_cmd <= queue_end)
	{
		if (*next_cmd == '\n' || *next_cmd == '\r' || *next_cmd == '\000') break;
		next_cmd++;
	}
	
	/* mark end of command */
	*next_cmd = '\000';
	
	/* make a copy of the command to be returned */
	message = strdup(message);
	if (message == NULL)
	{
		fprintf(stderr, "Out of memory.\n");
		exit(MALLOC_ERROR);
	}
	
	/* search for beginning of next command */
	next_cmd++;
	while(next_cmd <= queue_end)
	{
		if ((*next_cmd != '\n' && *next_cmd != '\r') || *next_cmd == '\000') break;
		next_cmd++;
	}
	
	/* if we reached end of queue free all */
	if (next_cmd >= queue_end)
	{
		free(cmd_queue);
		cmd_queue = NULL;
		next_cmd = NULL;
	}
	
	return(message);
}

int
enqueue_result(char *res)
{
	push_result(res, PERSISTENT_BUFFER);
	if (async_notice)
	{
		pthread_mutex_lock(&send_lock);
		write(server_socket, "R\r\n", 3);
		pthread_mutex_unlock(&send_lock);
		/* Don't send it again until a RESULT command is received */
		async_notice = 0;
	}
}

int
set_cmd_string_option(char **command, classad_context cad, const char *attribute, const char *option, const int quote_style)
{
	char *argument;
	char *to_append = NULL;
	char *new_command;
	int result;
	
	if ((result = classad_get_dstring_attribute(cad, attribute, &argument)) == C_CLASSAD_NO_ERROR)
	{
		if (strlen(argument) > 0)
		{
			if ((to_append = make_message(opt_format[quote_style], option, argument)) == NULL)
				result = C_CLASSAD_OUT_OF_MEMORY;
			free (argument);
		}
		else
			result = C_CLASSAD_VALUE_NOT_FOUND;
	}

	if (result == C_CLASSAD_NO_ERROR)
		if (new_command = (char *) realloc (*command, strlen(*command) + strlen(to_append) + 1))
		{
			strcat(new_command, to_append);
			*command = new_command;
		}
		else
			result = C_CLASSAD_OUT_OF_MEMORY;

	if (to_append) free (to_append);
	return(result);
}

int
set_cmd_int_option(char **command, classad_context cad, const char *attribute, const char *option, const int quote_style)
{
	int argument;
	char *to_append = NULL;
	char *new_command;
	int result;
	
	if ((result = classad_get_int_attribute(cad, attribute, &argument)) == C_CLASSAD_NO_ERROR)
	{
		if ((to_append = make_message(opt_format[quote_style], option, argument)) == NULL)
		{
			result = C_CLASSAD_OUT_OF_MEMORY;
		}
	}

	if (result == C_CLASSAD_NO_ERROR)
		if (new_command = (char *) realloc (*command, strlen(*command) + strlen(to_append) + 1))
		{
			strcat(new_command, to_append);
			*command = new_command;
		}
		else
			result = C_CLASSAD_OUT_OF_MEMORY;

	if (to_append) free (to_append);
	return(result);
}

int
set_cmd_bool_option(char **command, classad_context cad, const char *attribute, const char *option, const int quote_style)
{
	const char *str_yes = "yes";
	const char *str_no  = "no";
	int attr_value;
	char *argument;
	char *to_append = NULL;
	char *new_command;
	int result;
	
	if ((result = classad_get_bool_attribute(cad, attribute, &attr_value)) == C_CLASSAD_NO_ERROR)
	{
		argument = (char *)(attr_value ? str_yes : str_no);
		if ((to_append = make_message(opt_format[quote_style], option, argument)) == NULL)
			result = C_CLASSAD_OUT_OF_MEMORY;
	}

	if (result == C_CLASSAD_NO_ERROR)
		if (new_command = (char *) realloc (*command, strlen(*command) + strlen(to_append) + 1))
		{
			strcat(new_command, to_append);
			*command = new_command;
		}
		else
			result = C_CLASSAD_OUT_OF_MEMORY;

	if (to_append) free (to_append);
	return(result);
}


int
set_cmd_list_option(char **command, classad_context cad, const char *attribute, const char *option)
{
	char **list_cont;
	char **str_ptr;
	char *to_append = NULL;
	char *reallocated;
	int result;
	
	if ((result = classad_get_string_list_attribute(cad, attribute, &list_cont)) == C_CLASSAD_NO_ERROR)
	{
		if (to_append = strdup(option))
		{
			for (str_ptr = list_cont; (*str_ptr) != NULL; str_ptr++)
			{
				if (reallocated = (char *) realloc (to_append, strlen(*str_ptr) + strlen(to_append) + 2))
				{
					to_append = reallocated;
					strcat(to_append, " ");
					strcat(to_append, *str_ptr);
				}
				else
				{
					result = C_CLASSAD_OUT_OF_MEMORY;
					break;
				}					
			}
		}
		else /* strdup failed */
			result = C_CLASSAD_OUT_OF_MEMORY;

		classad_free_string_list(list_cont);
	}

	if (result == C_CLASSAD_NO_ERROR)
		if (reallocated = (char *) realloc (*command, strlen(*command) + strlen(to_append) + 1))
		{
			strcat(reallocated, to_append);
			*command = reallocated;
		}

	if (to_append) free (to_append);
	return(result);
}

char *
make_message(const char *fmt, ...)
{
	int n;
	char *result = NULL;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap) + 1;

	result = (char *) malloc (n);
	if (result)
		vsnprintf(result, n, fmt, ap);
	va_end(ap);

	return(result);
}

int
limit_proxy(char* proxyname)
{
	char *limcommand;
	char *tmpfilename;
	int res;
	char* globuslocation;
	globuslocation = (getenv("GLOBUS_LOCATION") ? getenv("GLOBUS_LOCATION") : "/opt/globus");
	tmpfilename = make_message("%s.lmt", proxyname);
	limcommand  = make_message("%s/bin/grid-proxy-init -limited -cert %s -key %s -out %s > /dev/null 2>&1",globuslocation, proxyname, proxyname, tmpfilename);
	res = system(limcommand);
	free(limcommand);
	/* If exitcode != 0 there may be a problem due to a warning by grid-proxy-init but */
	/* the call may have been successful. We just check the temporary proxy  */
	if (res)
	{
		limcommand = make_message("%s/bin/grid-proxy-info -f %s > /dev/null 2>&1", globuslocation, tmpfilename);
		res = system(limcommand);
		free(limcommand);
		/* If res is not null, we really got an error */
		if (res) return res;
	}
	if (!res) rename(tmpfilename, proxyname);
	free(tmpfilename);
	return res;
}

