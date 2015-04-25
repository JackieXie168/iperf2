/*--------------------------------------------------------------- 
 * Copyright (c) 1999,2000,2001,2002,2003                              
 * The Board of Trustees of the University of Illinois            
 * All Rights Reserved.                                           
 *--------------------------------------------------------------- 
 * Permission is hereby granted, free of charge, to any person    
 * obtaining a copy of this software (Iperf) and associated       
 * documentation files (the "Software"), to deal in the Software  
 * without restriction, including without limitation the          
 * rights to use, copy, modify, merge, publish, distribute,        
 * sublicense, and/or sell copies of the Software, and to permit     
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions: 
 *
 *     
 * Redistributions of source code must retain the above 
 * copyright notice, this list of conditions and 
 * the following disclaimers. 
 *
 *     
 * Redistributions in binary form must reproduce the above 
 * copyright notice, this list of conditions and the following 
 * disclaimers in the documentation and/or other materials 
 * provided with the distribution. 
 * 
 *     
 * Neither the names of the University of Illinois, NCSA, 
 * nor the names of its contributors may be used to endorse 
 * or promote products derived from this Software without
 * specific prior written permission. 
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
 * ________________________________________________________________
 * National Laboratory for Applied Network Research 
 * National Center for Supercomputing Applications 
 * University of Illinois at Urbana-Champaign 
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________ 
 * main.cpp
 * by Mark Gates <mgates@nlanr.net>
 * &  Ajay Tirumala <tirumala@ncsa.uiuc.edu>
 * -------------------------------------------------------------------
 * main does initialization and creates the various objects that will
 * actually run the iperf program, then waits in the Joinall().
 * -------------------------------------------------------------------
 * headers
 * uses
 *   <stdlib.h>
 *   <string.h>
 *
 *   <signal.h>
 * ------------------------------------------------------------------- */

#define HEADERS()

#include "headers.h"

#include "Settings.hpp"
#include "PerfSocket.hpp"
#include "Locale.h"
#include "Condition.h"
#include "Timestamp.hpp"
#include "Listener.hpp"
#include "delay.hpp"
#include "List.h"
#include "util.h"

#ifdef WIN32
#include "service.h"
#endif 

/* -------------------------------------------------------------------
 * prototypes
 * ------------------------------------------------------------------- */
// Function called at exit to clean up as much as possible
void cleanup( void );

/* -------------------------------------------------------------------
 * global variables
 * ------------------------------------------------------------------- */
extern "C" {
    // Global flag to signal a user interrupt
    int sInterupted = 0;
    // Global ID that we increment to be used 
    // as identifier for SUM reports
    int groupID = 0;
    // Mutex to protect access to the above ID
    Mutex groupCond;
    // Condition used to signify advances of the current
    // records being accessed in a report and also to
    // serialize modification of the report list
    Condition ReportCond;
    Condition ReportDoneCond;
}

// global variables only accessed within this file

// Thread that received the SIGTERM or SIGINT signal
// Used to ensure that if multiple threads receive the
// signal we do not prematurely exit
nthread_t sThread;
// The main thread uses this function to wait 
// for all other threads to complete
void waitUntilQuit( void );

const int MAXRATE = 1000000;
const int COMMAND_LENGTH = 1024;
const int MAXFUNC = 5;

const char *QDISC_ADD_FORMAT = "tc qdisc add dev %s root handle 1: htb default 2";
const char *QDISC_DELETE_FORMAT = "tc qdisc del dev %s root";
const char *CLASS_ADD_FORMAT = "tc class add dev %s parent 1:%d classid 1:%d htb rate %ldkbit ceil %ldkbit";
const char *CLASS_CHANGE_FORMAT = "tc class change dev %s parent 1:%d classid 1:%d htb rate %ldkbit ceil %ldkbit";
const char *FILTER_ADD_FORMAT = "tc filter add dev %s parent 1:0 pref %d protocol ip u32 match u32 0x%x 0xffffffff at 12 match u32 0x%x 0xffffffff at 16";
const char *FILTER_PROTOCOL_FORMAT = " match ip protocol 0x%x 0xff";
const char *FILTER_SPORT_FORMAT = " match u16 0x%x 0xffff at 20";
const char *FILTER_DPORT_FORMAT = " match u16 0x%x 0xffff at 22";
const char *FILTER_END = " flowid 1:%d";

/* -------------------------------------------------------------------
 * main()
 *      Entry point into Iperf
 *
 * sets up signal handlers
 * initialize global locks and conditions
 * parses settings from environment and command line
 * starts up server or client thread
 * waits for all threads to complete
 * ------------------------------------------------------------------- */
int main( int argc, char **argv ) {

    // Set SIGTERM and SIGINT to call our user interrupt function
    my_signal( SIGTERM, Sig_Interupt );
    my_signal( SIGINT,  Sig_Interupt );
    my_signal( SIGALRM,  Sig_Interupt );

#ifndef WIN32
    // Ignore broken pipes
    signal(SIGPIPE,SIG_IGN);
#else
    // Start winsock
    WSADATA wsaData;
    int rc = WSAStartup( 0x202, &wsaData );
    WARN_errno( rc == SOCKET_ERROR, "WSAStartup" );
	if (rc == SOCKET_ERROR)
		return 0;

    // Tell windows we want to handle our own signals
    SetConsoleCtrlHandler( sig_dispatcher, true );
#endif

    // Initialize global mutexes and conditions
    Condition_Initialize ( &ReportCond );
    Condition_Initialize ( &ReportDoneCond );
    Mutex_Initialize( &groupCond );
    Mutex_Initialize( &clients_mutex );

    // Initialize the thread subsystem
    thread_init( );

    // Initialize the interrupt handling thread to 0
    sThread = thread_zeroid();

    // perform any cleanup when quitting Iperf
    atexit( cleanup );

    // Allocate the "global" settings
    thread_Settings* ext_gSettings = new thread_Settings;

    // Initialize settings to defaults
    Settings_Initialize( ext_gSettings );
    // read settings from environment variables
    Settings_ParseEnvironment( ext_gSettings );
    // read settings from command-line parameters
    Settings_ParseCommandLine( argc, argv, ext_gSettings );
	
	if(ext_gSettings->mTCStream)
	{
		dev_init(ext_gSettings->mDev);
	}

    // Check for either having specified client or server
    if ( ext_gSettings->mThreadMode == kMode_Client 
         || ext_gSettings->mThreadMode == kMode_Listener ) {
#ifdef WIN32
        // Start the server as a daemon
        // Daemon mode for non-windows in handled
        // in the listener_spawn function
        if ( isDaemon( ext_gSettings ) ) {
            CmdInstallService(argc, argv);
            return 0;
        }

        // Remove the Windows service if requested
        if ( isRemoveService( ext_gSettings ) ) {
            // remove the service
            if ( CmdRemoveService() ) {
                fprintf(stderr, "IPerf Service is removed.\n");

                return 0;
            }
        }
#endif
        // initialize client(s)
        if ( ext_gSettings->mThreadMode == kMode_Client ) {
            client_init( ext_gSettings );
        }

#ifdef HAVE_THREAD
        // start up the reporter and client(s) or listener
        {
            thread_Settings *into = NULL;
            // Create the settings structure for the reporter thread
            Settings_Copy( ext_gSettings, &into );
            into->mThreadMode = kMode_Reporter;

            // Have the reporter launch the client or listener
            into->runNow = ext_gSettings;
            
            // Start all the threads that are ready to go
            thread_start( into );
        }
#else
        // No need to make a reporter thread because we don't have threads
        thread_start( ext_gSettings );
#endif
    } else {
        // neither server nor client mode was specified
        // print usage and exit

#ifdef WIN32
        // In Win32 we also attempt to start a previously defined service
        // Starting in 2.0 to restart a previously defined service
        // you must call iperf with "iperf -D" or using the environment variable
        SERVICE_TABLE_ENTRY dispatchTable[] =
        {
            { TEXT(SZSERVICENAME), (LPSERVICE_MAIN_FUNCTION)service_main},
            { NULL, NULL}
        };

        // Only attempt to start the service if "-D" was specified
        if ( !isDaemon(ext_gSettings) ||
             // starting the service by SCM, there is no arguments will be passed in.
             // the arguments will pass into Service_Main entry.
             !StartServiceCtrlDispatcher(dispatchTable) )
            // If the service failed to start then print usage
#endif
        fprintf( stderr, usage_short, argv[0], argv[0] );

        return 0;
    }

    // wait for other (client, server) threads to complete
    thread_joinall();
    
    // all done!
    return 0;
} // end main

/* -------------------------------------------------------------------
 * Signal handler sets the sInterupted flag, so the object can
 * respond appropriately.. [static]
 * ------------------------------------------------------------------- */

void Sig_Interupt( int inSigno ) {
#ifdef HAVE_THREAD
    // We try to not allow a single interrupt handled by multiple threads
    // to completely kill the app so we save off the first thread ID
    // then that is the only thread that can supply the next interrupt
    if ( thread_equalid( sThread, thread_zeroid() ) ) {
        sThread = thread_getid();
    } else if ( thread_equalid( sThread, thread_getid() ) ) {
        sig_exit( inSigno );
    }

    // global variable used by threads to see if they were interrupted
    sInterupted = 1;

    // with threads, stop waiting for non-terminating threads
    // (ie Listener Thread)
    thread_release_nonterm( 1 );

#else
    // without threads, just exit quietly, same as sig_exit()
    sig_exit( inSigno );
#endif
}

/* -------------------------------------------------------------------
 * Any necesary cleanup before Iperf quits. Called at program exit,
 * either by exit() or terminating main().
 * ------------------------------------------------------------------- */

void cleanup( void ) {
#ifdef WIN32
    // Shutdown Winsock
    WSACleanup();
#endif
    // clean up the list of clients
    Iperf_destroy ( &clients );

    // shutdown the thread subsystem
    thread_destroy( );
} // end cleanup

//split the string into commands array
void split(char *commands[], char *str, char *delim, int num)
{
    int i = 0;
    char *buff = str;
    while ( (commands[i] = strtok(buff, delim) ) != NULL) 
	{
        i++;
        buff = NULL;
        if (i == num-1)
            break;
    }
    commands[i] = NULL;
}


void dev_init(const char *dev) {
	char qdisc_command[COMMAND_LENGTH];
	char *qcommands[32];

	snprintf(qdisc_command, COMMAND_LENGTH, QDISC_DELETE_FORMAT, dev);
	split(qcommands, qdisc_command, " ", 32);
	execute(qcommands);

	snprintf(qdisc_command, COMMAND_LENGTH, QDISC_ADD_FORMAT, dev);
	split(qcommands, qdisc_command, " ", 32);
	execute(qcommands);
		
	char class_command[COMMAND_LENGTH];
	char *ccommands[64];

	snprintf(class_command, COMMAND_LENGTH, CLASS_ADD_FORMAT, dev, 0, 1, MAXRATE, MAXRATE);
	split(ccommands, class_command, " ", 64);
	execute(ccommands);
	
	snprintf(class_command, COMMAND_LENGTH, CLASS_ADD_FORMAT, dev, 1, 2, 1, MAXRATE);
	split(ccommands, class_command, " ", 64);
	execute(ccommands);
}

// add flow to tc
void add_flow(const char *dev,const struct tcFlow *flow) {
	char class_command[COMMAND_LENGTH];
	char *ccommands[64];

	snprintf(class_command, COMMAND_LENGTH, CLASS_ADD_FORMAT,
			 dev, 1, flow->id, flow->rate, flow->rate);
	split(ccommands, class_command, " ", 64);
	execute(ccommands);

	char filter_command[COMMAND_LENGTH];
	char *fcommands[128];
	char str[128];

	snprintf(filter_command, COMMAND_LENGTH, FILTER_ADD_FORMAT,
             dev, flow->id, htonl(flow->srcip), htonl(flow->dstip));
    if (flow->protocol)
	{
        snprintf(str, 128, FILTER_PROTOCOL_FORMAT, flow->protocol);
        strcat(filter_command, str);
    }

	if (flow->dport) {
        snprintf(str,128, FILTER_DPORT_FORMAT, flow->dport);
		strcat(filter_command, str);
    }

	snprintf(str, 128, FILTER_END, flow->id);
	strcat(filter_command, str);
	
	split(fcommands, filter_command, " ", 128);
	execute(fcommands);
}

//change current flow
void change_flow(const char *dev,const struct tcFlow *flow) {
	char class_command[COMMAND_LENGTH];
	char *ccommands[64];

	snprintf(class_command, COMMAND_LENGTH, CLASS_CHANGE_FORMAT,
			 dev, 1, flow->id, flow->rate, flow->rate);
	split(ccommands, class_command, " ", 64);
	execute(ccommands);
}

static int my_read(int fd, char *err_out,int num)
{
    int n;
    char buff[100];
    memset(err_out, 0, num);
    while ( (n = read(fd, buff, 99)) > 0 ) {
        buff[99] = '\0';
        strncat(err_out, buff, num);
        num -= n;
    }

    close(fd);
    return n;
}

void CHECK(int cond, char string[])
{
    if (cond) {
        perror(string);
        exit(-1);
    }
}


void execute(char *commands[])
{
	char ** c= commands;
	int ret;
	int fd[2];
	pid_t pid;

	for (; *c != NULL; c++)
	{
		printf("%s  ", *c);
	}
	printf("\n");

	ret = pipe(fd);
	CHECK(ret < 0, "pipe error");

	pid = fork();
    CHECK(pid < 0, "fork error");

    if( pid > 0) 
	{
		char out[1024];

		close(fd[1]);

		my_read(fd[0], out, 1024);
		out[1024] = '\0';

        if (strlen(out) != 0)
            printf("err = %s",out);
            return;
        }

        close(fd[0]);

        if (fd[1] != STDERR_FILENO) {
            dup2(fd[1], STDERR_FILENO);
            close(fd[1]);
        }

        execv("/sbin/tc",commands);
}

double getRate(int ChoseFunction, double intval)
{
	double RandNumber;
	double ReturnNumber;
	const double unit = 1000*1000;
	static double GivenTime = 0;
	time_t t;
	time(&t);

   	RandNumber = ((rand_r((unsigned int *)&t)) % 201 - 50) / 20.0;
	printf("\nrandNumber is %.2f \n", RandNumber);
    switch(ChoseFunction)
    {
        case(1):  //k=1 b=0
        {
            ReturnNumber = GivenTime + RandNumber;
            break;
        }
        case(2):    //k=-1 b=0
        {
            ReturnNumber = -GivenTime + 120 + RandNumber;
            break;
        }
        case(3):
        {
            ReturnNumber =  GivenTime * (GivenTime - 100) / - 25.0 + RandNumber;
            break;
        }
        case(4):
        {
        	if(GivenTime <= 1)
        		GivenTime == 1 + abs(RandNumber);
            ReturnNumber = 2 * log(GivenTime -1) / log(1.11) + RandNumber;
            break;
        }
        case(5):
        {
            ReturnNumber = abs(50 * sin(GivenTime / 8.3) + RandNumber);
			break;
        }
    }

	GivenTime += intval;
	printf("GivenTime : %.2f\n", GivenTime);
	printf("ReturnNumer : %.2f\n",ReturnNumber);	
	if(ReturnNumber > 0)
		return ReturnNumber*unit;
	else
		return 0;
}


double getFileRate(char *filename)
{
	static int index = 0;
	double ret = 0;
	/*
	if(!index)
	{	
		char *line;
		FILE* rateFile;
		int read_size,i=0,len=0;
		if(rateFile = fopen(filename, "r") ==NULL)
			perror("open file error!\n");
		while ((read_size = getline(&line, &len, rateFile)) != 1) 
		{
    		printf("Retrieved line of length %zu :\n", read_size);
        	printf("%s", line);
        	filerate[i++] = atof(line);
    	}
	    close(rateFile);
	}
	ret = filerate[index];
    index = (index+1) % 2048;
    */
    return ret;
}


void randomDelay()
{
	unsigned int dTime;
	unsigned long dUsecs;
	srand((int)time(0));
	dTime = rand()%1000;
	if(dTime < 100)
		dTime = 100;
	dUsecs = ( (double) dTime/100.0 ) * 1000000 * 60;
//	delay_loop(dUsecs);
	sleep(dTime);	
}

int genChoFunc()
{
	int chofunc;
	srand((int)time(0));
	chofunc = rand()%MAXFUNC + 1;
	printf("Chosed NetFlow Function: %d \n", chofunc);
	return chofunc;
}
