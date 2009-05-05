#include "config.h"

#include <stdio.h>
#include <atl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include "evpath_old.h"
#include "gen_thread.h"
#ifdef HAVE_WINDOWS_H
#include <windows.h>
#define drand48() (((double)rand())/((double)RAND_MAX))
#define lrand48() rand()
#define srand48(x)
#else
#include <sys/wait.h>
#endif

typedef struct _complex_rec {
    double r;
    double i;
} complex, *complex_ptr;

typedef struct _nested_rec {
    complex item;
} nested, *nested_ptr;

static IOField nested_field_list[] =
{
    {"item", "complex", sizeof(complex), IOOffset(nested_ptr, item)},
    {NULL, NULL, 0, 0}
};

static IOField complex_field_list[] =
{
    {"r", "double", sizeof(double), IOOffset(complex_ptr, r)},
    {"i", "double", sizeof(double), IOOffset(complex_ptr, i)},
    {NULL, NULL, 0, 0}
};

typedef struct _simple_rec {
    int integer_field;
    short short_field;
    long long_field;
    nested nested_field;
    double double_field;
    char char_field;
    int scan_sum;
} simple_rec, *simple_rec_ptr;

static IOField simple_field_list[] =
{
    {"integer_field", "integer",
     sizeof(int), IOOffset(simple_rec_ptr, integer_field)},
    {"short_field", "integer",
     sizeof(short), IOOffset(simple_rec_ptr, short_field)},
    {"long_field", "integer",
     sizeof(long), IOOffset(simple_rec_ptr, long_field)},
    {"nested_field", "nested",
     sizeof(nested), IOOffset(simple_rec_ptr, nested_field)},
    {"double_field", "float",
     sizeof(double), IOOffset(simple_rec_ptr, double_field)},
    {"char_field", "char",
     sizeof(char), IOOffset(simple_rec_ptr, char_field)},
    {"scan_sum", "integer",
     sizeof(int), IOOffset(simple_rec_ptr, scan_sum)},
    {NULL, NULL, 0, 0}
};

static CMFormatRec simple_format_list[] =
{
    {"simple", simple_field_list},
    {"complex", complex_field_list},
    {"nested", nested_field_list},
    {NULL, NULL}
};

static IOField filter_field_list[] =
{
    {"integer_field", "integer",
     sizeof(int), IOOffset(simple_rec_ptr, integer_field)},
    {NULL, NULL, 0, 0}
};

static CMFormatRec filter_format_list[] =
{
    {"simple", filter_field_list},
    {NULL, NULL}
};

static
void 
generate_record(event)
simple_rec_ptr event;
{
    long sum = 0;
    event->integer_field = (int) lrand48() % 100;
    sum += event->integer_field % 100;
    event->short_field = ((short) lrand48());
    sum += event->short_field % 100;
    event->long_field = ((long) lrand48());
    sum += event->long_field % 100;

    event->nested_field.item.r = drand48();
    sum += ((int) (event->nested_field.item.r * 100.0)) % 100;
    event->nested_field.item.i = drand48();
    sum += ((int) (event->nested_field.item.i * 100.0)) % 100;

    event->double_field = drand48();
    sum += ((int) (event->double_field * 100.0)) % 100;
    event->char_field = lrand48() % 128;
    sum += event->char_field;
    sum = sum % 100;
    event->scan_sum = (int) sum;
}

int quiet = 1;

typedef struct _client_rec {
    int output_index;
    int *message_count;
} *client_data_ptr;


static
int
simple_handler(cm, vevent, client_data, attrs)
CManager cm;
void *vevent;
void *client_data;
attr_list attrs;
{
    simple_rec_ptr event = vevent;
    client_data_ptr cdata = (client_data_ptr)client_data;
    int output_index = cdata->output_index;
    long sum = 0, scan_sum = 0;
    sum += event->integer_field % 100;
    sum += event->short_field % 100;
    sum += event->long_field % 100;
    sum += ((int) (event->nested_field.item.r * 100.0)) % 100;
    sum += ((int) (event->nested_field.item.i * 100.0)) % 100;
    sum += ((int) (event->double_field * 100.0)) % 100;
    sum += event->char_field;
    sum = sum % 100;
    scan_sum = event->scan_sum;
    if (sum != scan_sum) {
	printf("Received record checksum does not match. expected %d, got %d\n",
	       (int) sum, (int) scan_sum);
    }
    if ((quiet <= 0) || (sum != scan_sum)) {
	printf("Received data from output index %d\n", output_index);
	printf("In the handler, event data is :\n");
	printf("	integer_field = %d\n", event->integer_field);
	printf("	short_field = %d\n", event->short_field);
	printf("	long_field = %ld\n", event->long_field);
	printf("	double_field = %g\n", event->double_field);
	printf("	char_field = %c\n", event->char_field);
	printf("Data was received with attributes : \n");
	if (attrs) dump_attr_list(attrs);
    }
    if (cdata->message_count != NULL) {
	int tmp = cdata->message_count[output_index];
	cdata->message_count[output_index] = tmp + 1;
    }
    return 0;
}

static int do_regression_master_test();
static int regression = 1;
static int repeat_count = 10;
static char *router_func = "{int ret = input.long_field % 2; \n\
static int count = 0;\n\
if (ret == 0) { return -1;}\n\
count++;\n\
if (count < 6) return 0;\n\
if (count < 9) return 1;\n\
return 2;\n\
}\0\0";

int
main(argc, argv)
int argc;
char **argv;
{
    CManager cm;
    int regression_master = 1;

    while (argv[1] && (argv[1][0] == '-')) {
	if (argv[1][1] == 'c') {
	    regression_master = 0;
	} else if (argv[1][1] == 's') {
	    regression_master = 0;
	} else if (argv[1][1] == 'q') {
	    quiet++;
	} else if (argv[1][1] == 'v') {
	    quiet--;
	} else if (argv[1][1] == 'n') {
	    regression = 0;
	    quiet = -1;
	}
	argv++;
	argc--;
    }
    srand48(getpid());
#ifdef USE_PTHREADS
    gen_pthread_init();
#endif
    if (regression && regression_master) {
	return do_regression_master_test();
    }
    cm = CManager_create();
/*    (void) CMfork_comm_thread(cm);*/

    if (argc == 1) {
	attr_list contact_list, listen_list = NULL;
	char *transport = NULL;
	char *postfix = NULL;
	char *string_list;
	struct _client_rec rec0, rec1, rec2;
	EVstone term0, term1, term2;
	if ((transport = getenv("CMTransport")) != NULL) {
	    if (listen_list == NULL) listen_list = create_attr_list();
	    add_attr(listen_list, CM_TRANSPORT, Attr_String,
		     (attr_value) strdup(transport));
	}
	if ((postfix = getenv("CMNetworkPostfix")) != NULL) {
	    if (listen_list == NULL) listen_list = create_attr_list();
	    add_attr(listen_list, CM_NETWORK_POSTFIX, Attr_String,
		     (attr_value) strdup(postfix));
	}
	CMlisten_specific(cm, listen_list);
	contact_list = CMget_contact_list(cm);
	if (contact_list) {
	    string_list = attr_list_to_string(contact_list);
	} else {
	    /* must be multicast, hardcode a contact list */
#define HELLO_PORT 12345
#define HELLO_GROUP "225.0.0.37"
	    int addr;
	    (void) inet_aton(HELLO_GROUP, (struct in_addr *)&addr);
	    contact_list = create_attr_list();
	    add_attr(contact_list, CM_MCAST_ADDR, Attr_Int4,
		     (attr_value) (long)addr);
	    add_attr(contact_list, CM_MCAST_PORT, Attr_Int4,
		     (attr_value) HELLO_PORT);
	    add_attr(contact_list, CM_TRANSPORT, Attr_String,
		     (attr_value) "multicast");
/*	    conn = CMinitiate_conn(cm, contact_list);*/
	    string_list = attr_list_to_string(contact_list);
	    free_attr_list(contact_list);
	}	
	term0 = EValloc_stone(cm);
	rec0.output_index = 0;
	rec0.message_count = NULL;
	EVassoc_terminal_action(cm, term0, simple_format_list, 
				simple_handler, (void*)&rec0);
	term1 = EValloc_stone(cm);
	rec1.output_index = 1;
	rec1.message_count = NULL;
	EVassoc_terminal_action(cm, term1, simple_format_list, 
				simple_handler, (void*)&rec1);
	term2 = EValloc_stone(cm);
	rec2.output_index = 2;
	rec2.message_count = NULL;
	EVassoc_terminal_action(cm, term2, simple_format_list, 
				simple_handler, (void*)&rec2);
	
	printf("Contact list \"%d:%d:%d:%s\"\n", term0, term1, term2, string_list);
	CMsleep(cm, 120);
    } else {
	simple_rec data;
	attr_list attrs;
	int remote_stone0, remote_stone1, remote_stone2, stone = 0;
	EVstone term0 = -1, term1 = -1, term2 = -1;
	int count;
	EVsource source_handle;
	char *filter;
	EVaction faction;
	atom_t CMDEMO_TEST_ATOM = attr_atom_from_string("CMdemo_test_atom");
	if (argc == 2) {
	    attr_list contact_list;
	    char *list_str;
	    sscanf(argv[1], "%d:%d:%d", &remote_stone0, &remote_stone1, &remote_stone2);
	    list_str = strchr(argv[1], ':') + 1;
	    list_str = strchr(list_str, ':') + 1;
	    list_str = strchr(list_str, ':') + 1;
	    contact_list = attr_list_from_string(list_str);
	    term0 = EValloc_stone(cm);
	    EVassoc_output_action(cm, term0, contact_list, remote_stone0);
	    term1 = EValloc_stone(cm);
	    contact_list = attr_list_from_string(list_str);
	    EVassoc_output_action(cm, term1, contact_list, remote_stone1);
	    term2 = EValloc_stone(cm);
	    contact_list = attr_list_from_string(list_str);
	    EVassoc_output_action(cm, term2, contact_list, remote_stone2);
	}
	filter = create_router_action_spec(filter_format_list, router_func);
    
	stone = EValloc_stone(cm);
	faction = EVassoc_immediate_action(cm, stone, filter, NULL);
	EVaction_set_output(cm, stone, faction, 0, term0);
	EVaction_set_output(cm, stone, faction, 1, term1);
	EVaction_set_output(cm, stone, faction, 2, term2);

	attrs = create_attr_list();
	add_attr(attrs, CMDEMO_TEST_ATOM, Attr_Int4, (attr_value)45678);
	source_handle = EVcreate_submit_handle(cm, stone, simple_format_list);
	if (quiet <= 0) printf("submitting %d\n", data.integer_field);
	count = repeat_count;
	while (count != 0) {
	    generate_record(&data);
	    if (quiet <=0) {printf("submitting %ld\n", data.long_field);}
	    EVsubmit(source_handle, &data, attrs);
	    if ((data.long_field%2 == 1) && (count != -1)) {
		count--;
	    }
	}
	CMsleep(cm, 10);
	free_attr_list(attrs);
    }
    CManager_close(cm);
    return 0;
}

static pid_t subproc_proc = 0;

static void
fail_and_die(signal)
int signal;
{
    fprintf(stderr, "EVtest failed to complete in reasonable time\n");
    if (subproc_proc != 0) {
	kill(subproc_proc, 9);
    }
    exit(1);
}

static
pid_t
run_subprocess(args)
char **args;
{
#ifdef HAVE_WINDOWS_H
    int child;
    child = _spawnv(_P_NOWAIT, "./router_test2.exe", args);
    if (child == -1) {
	printf("failed for router_test2\n");
	perror("spawnv");
    }
    return child;
#else
#ifndef NOTDEF
    pid_t child;
    if (quiet <=0) {printf("Forking subprocess\n");}
    child = fork();
    if (child == 0) {
	/* I'm the child */
	execv("./router_test2", args);
    }
    return child;
#else
    printf("nothing doing  %s %s\n", args[1], args[2]);
    sleep(20);
#endif
#endif
}

static int
do_regression_master_test()
{
    CManager cm;
    char *args[] = {"router_test2", "-c", NULL, NULL, NULL};
    int exit_state;
    int ret;
    int forked = 0;
    attr_list contact_list, listen_list = NULL;
    char *string_list, *transport, *postfix;
    int message_counts[3], i;
    EVstone term0, term1, term2;
    struct _client_rec rec0, rec1, rec2;
#ifdef HAVE_WINDOWS_H
    SetTimer(NULL, 5, 1000, (TIMERPROC) fail_and_die);
#else
    struct sigaction sigact;
    sigact.sa_flags = 0;
    sigact.sa_handler = fail_and_die;
    sigemptyset(&sigact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGALRM);
    sigaction(SIGALRM, &sigact, NULL);
    alarm(300);
#endif
    cm = CManager_create();
    forked = CMfork_comm_thread(cm);
    if ((transport = getenv("CMTransport")) != NULL) {
	listen_list = create_attr_list();
	add_attr(listen_list, CM_TRANSPORT, Attr_String,
		 (attr_value) strdup(transport));
    }
    if ((postfix = getenv("CMNetworkPostfix")) != NULL) {
	if (listen_list == NULL) listen_list = create_attr_list();
	add_attr(listen_list, CM_NETWORK_POSTFIX, Attr_String,
		 (attr_value) strdup(postfix));
    }
    CMlisten_specific(cm, listen_list);
    contact_list = CMget_contact_list(cm);
    if (contact_list) {
	string_list = attr_list_to_string(contact_list);
	free_attr_list(contact_list);
    } else {
	/* must be multicast, hardcode a contact list */
#define HELLO_PORT 12345
#define HELLO_GROUP "225.0.0.37"
	int addr;
	(void) inet_aton(HELLO_GROUP, (struct in_addr *)&addr);
	contact_list = create_attr_list();
	add_attr(contact_list, CM_MCAST_ADDR, Attr_Int4,
		 (attr_value) (long)addr);
	add_attr(contact_list, CM_MCAST_PORT, Attr_Int4,
		 (attr_value) HELLO_PORT);
	add_attr(contact_list, CM_TRANSPORT, Attr_String,
		 (attr_value) "multicast");
	(void) CMinitiate_conn(cm, contact_list);
	string_list = attr_list_to_string(contact_list);
	free_attr_list(contact_list);
    }	

    if (quiet <= 0) {
	if (forked) {
	    printf("Forked a communication thread\n");
	} else {
	    printf("Doing non-threaded communication handling\n");
	}
    }
    srand48(1);

    term0 = EValloc_stone(cm);
    message_counts[0] = message_counts[1] = message_counts[2] = 0;
    rec0.output_index = 0;
    rec0.message_count = &message_counts[0];
    EVassoc_terminal_action(cm, term0, simple_format_list, simple_handler, &rec0);
    term1 = EValloc_stone(cm);
    rec1.output_index = 1;
    rec1.message_count = &message_counts[0];
    EVassoc_terminal_action(cm, term1, simple_format_list, simple_handler, &rec1);
    term2 = EValloc_stone(cm);
    rec2.output_index = 2;
    rec2.message_count = &message_counts[0];
    EVassoc_terminal_action(cm, term2, simple_format_list, simple_handler, &rec2);
    i = 2;
    if (!quiet) args[i++] = "-v";
    args[i] = malloc(20 + strlen(string_list));
    sprintf(args[i++], "%d:%d:%d:%s", term0, term1, term2, string_list);
    subproc_proc = run_subprocess(args);

    /* give him time to start */
    CMsleep(cm, 10);
/* stuff */
    if (quiet <= 0) {
	printf("Waiting for remote....\n");
    }
#ifdef HAVE_WINDOWS_H
    if (_cwait(&exit_state, subproc_proc, 0) == -1) {
	perror("cwait");
    }
    if (exit_state == 0) {
	if (quiet <= 0) 
	    printf("Passed single remote subproc test\n");
    } else {
	printf("Single remote subproc exit with status %d\n",
	       exit_state);
    }
#else
    if (waitpid(subproc_proc, &exit_state, 0) == -1) {
	perror("waitpid");
    }
    if (WIFEXITED(exit_state)) {
	if (WEXITSTATUS(exit_state) == 0) {
	    if (quiet <- 1) 
		printf("Passed single remote subproc test\n");
	} else {
	    printf("Single remote subproc exit with status %d\n",
		   WEXITSTATUS(exit_state));
	}
    } else if (WIFSIGNALED(exit_state)) {
	printf("Single remote subproc died with signal %d\n",
	       WTERMSIG(exit_state));
    }
#endif
    free(string_list);
    CManager_close(cm);
    ret = 0;
    if (message_counts[0] != 5) {
	printf("Message count[0] == %d\n", message_counts[0]);
	ret = 1;
    }
    if (message_counts[1] != 3) {
	printf("Message count[1] == %d\n", message_counts[1]);
	ret = 1;
    }
    if (message_counts[2] != 2) {
	printf("Message count[2] == %d\n", message_counts[2]);
	ret = 1;
    }
    return ret;
}
