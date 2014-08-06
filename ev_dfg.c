#include "config.h"
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <strings.h>

#include "cod.h"
#include "evpath.h"
#include "cm_internal.h"
#include "ev_deploy.h"
#include "revpath.h"
#include "ev_dfg_internal.h"
#include <assert.h>

/*
 *  Some notes about the (current) operation of EVdfg.
 *  
 *  EVdfg operation is based on CM-level messages.  It currently supports
 *  only one DFG per process/CM (I.E. there is no DFG identifier passed in
 *  any operation, so the DFG is implied by the contacted CM).  DFGs pass
 *  through several states, including : Joining (while waiting for
 *  participating nodes), Starting (between last node join and actual
 *  operation), Running (normal operation), Reconfiguring (master changing
 *  DFG structure), and Shutdown (all nodes have voted for shutdown and
 *  we're killing the system).  (State is maintained in the master, and is
 *  not reliable in the client nodes.)
 *  
 *  Generally, most operation triggers have the same sort of structure.
 *  I.E. they check to see if they are the master.  If they are, the perform
 *  the operation directly, and if not they send a message to the master.
 *  The message handler then does the subroutine call that performs the
 *  operation directly.  So, the message handler and the wrapper subroutine
 *  are structured similarly for most operations.  However, some are handled
 *  differently.  In particular, notification of client departure
 *  (connection/node failure) and client join can't be handled except when
 *  we're in the Running state.  (Well, Join is handled in Joining too.)
 *  Also, voluntary reconfiguration, in addition to only being handled in
 *  the Running state, *must* be queued for later handling if it happens
 *  locally (because it's triggered by a call inside a CoD event handler,
 *  and reconfiguring while running inside a handler seems bad).  So, those
 *  messages must be queued for later handling.
 *
 *  Currently, we handle each of these messages on transition from another
 *  state into Running, or, in the case of voluntary reconfiguration if
 *  triggered locally, we handle it inside a CM delayed task (which should
 *  assure that we're at least at a message handling point).
 */


char *str_state[] = {"DFG_Joining", "DFG_Starting", "DFG_Running", "DFG_Reconfiguring", "DFG_Shutting_Down"};
static char *master_msg_str[] = {"DFGnode_join", "DFGdeploy_ack", "DFGshutdown_contrib", "DFGconn_shutdown", 
			  "DFGflush_reconfig", NULL};


static void handle_conn_shutdown(EVdfg_master master, EVdfg_master_msg_ptr msg);
static void handle_node_join(EVdfg_master master, EVdfg_master_msg_ptr msg);
static void handle_flush_reconfig(EVdfg_master master, EVdfg_master_msg_ptr);
static void handle_deploy_ack(EVdfg_master master, EVdfg_master_msg_ptr);
static void handle_shutdown_contrib(EVdfg_master master, EVdfg_master_msg_ptr);

static void
queue_master_msg(EVdfg_master master, void*vmsg, EVmaster_msg_type msg_type, CMConnection conn, int copy);
static void free_master_msg(EVdfg_master_msg *msg);

static void free_attrs_msg(EVflush_attrs_reconfig_ptr msg);
static FMStructDescRec EVdfg_conn_shutdown_format_list[];
static FMStructDescRec EVdfg_deploy_ack_format_list[];
static FMStructDescRec EVdfg_flush_attrs_reconfig_format_list[];
static FMStructDescRec EVdfg_node_join_format_list[];
static FMStructDescRec EVdfg_ready_format_list[];
static FMStructDescRec EVdfg_deploy_format_list[];
static FMStructDescRec EVdfg_deploy_ack_format_list[];
static FMStructDescRec EVdfg_shutdown_format_list[];
static FMStructDescRec EVdfg_shutdown_contribution_format_list[];


/* msg action model
 *
 For each state/ for each master msg one of these possibilities:
	H - handle - dequeue and call handler (may change state, start over )
	U - unexpected - immediate error and discard (continue to next )
	I - ignore - discard (continue to next )
	L - leave_queued - (continue to next )
*/
static
char action_model[DFG_Last_State][DFGlast_msg] = {
/* join		deploy_ack	shutdown_contrib	conn_shutdown	flush_reconfig */
  {'H', 	'U',	 	'U', 			'U', 		'U'},/* state Joining */
  {'Q',		'H',		'H',			'Q',		'Q'},/* state Starting */
  {'H',		'U',		'H',			'H',		'H'},/* state Running */
  {'Q',		'H',		'H',			'Q',		'Q'},/* state Reconfiguring */
  {'U',		'U',		'U',			'I',		'U'}/* state Shutting Down */
};

typedef void (*master_msg_handler_func) (EVdfg_master master, EVdfg_master_msg_ptr msg);
static master_msg_handler_func master_msg_handler[DFGlast_msg] = {handle_node_join, handle_deploy_ack, handle_shutdown_contrib, handle_conn_shutdown, handle_flush_reconfig};
static void dfg_master_msg_handler(CManager cm, CMConnection conn, void *vmsg, 
				   void *client_data, attr_list attrs);

static void
handle_queued_messages(CManager cm, void* vmaster)
{
    /* SHOULD */
    /*  1 - consolidate node failure messages (likely to get several for each node) and handle these first */
    /*  2 -  handle node join messages last */
    /* FOR THE MOMENT */
    /* just do everything in order */
    /* beware the the list might change while we're running a handler */
    EVdfg_master master = (EVdfg_master) vmaster;
    EVdfg_master_msg_ptr next;
    EVdfg_master_msg_ptr *last_ptr;

    if (master->queued_messages == NULL) return;
    next = master->queued_messages;
    last_ptr = &master->queued_messages;
    while(next != NULL) {
	CMtrace_out(cm, EVdfgVerbose, "EVDFG handle_queued_messages -  master DFG state is %s\n", str_state[master->state]);
	switch (action_model[master->state][next->msg_type]) {
	case 'H':
	    CMtrace_out(cm, EVdfgVerbose, "Master Message is type %s, calling handler\n", master_msg_str[next->msg_type]);
	    *last_ptr = next->next;  /* remove msg from queue */
	    (*master_msg_handler[next->msg_type])(master, next);
	    free_master_msg(next);
	    next = master->queued_messages;   /* start from scratch in case state changed */
	    break;
	case 'U':
	    printf("Master Message is type %s, UNEXPECTED!  Discarding...\n", master_msg_str[next->msg_type]);
	    *last_ptr = next->next;  /* remove msg from queue */
	    free_master_msg(next);
	    next = *last_ptr;
	    break;
	case 'Q':
	    printf("Master Message is type %s, not appropriate now, leaving queued...\n", master_msg_str[next->msg_type]);
	    next = next->next;
	    break;
	default:
	    printf("Unexpected action type '%c', discarding\n", action_model[master->state][next->msg_type]);
	    /* falling through */
	case 'I':
	    *last_ptr = next->next;  /* remove msg from queue */
	    free_master_msg(next);
	    next = *last_ptr;
	    break;
	}
	CMtrace_out(cm, EVdfgVerbose, "EVDFG handle queued end loop -  master DFG state is now %s\n", str_state[master->state]);
    }	    
    CMtrace_out(cm, EVdfgVerbose, "EVDFG handle queued exiting -  master DFG state is now %s\n", str_state[master->state]);
}

static void
handle_queued_messages_lock(CManager cm, void* vmaster)
{
    CManager_lock(cm);
    handle_queued_messages(cm, vmaster);
    CManager_unlock(cm);
}

EVdfg_stone
INT_EVdfg_create_source_stone(EVdfg dfg, char *source_name)
{
    int len = strlen(source_name) + strlen("source:");
    char *act = malloc(len + 1);
    strcpy(stpcpy(&act[0], "source:"), source_name);
    return INT_EVdfg_create_stone(dfg, &act[0]);
}

extern void 
INT_EVdfg_add_sink_action(EVdfg_stone stone, char *sink_name)
{
    int len = strlen(sink_name) + strlen("sink:");
    char *act = malloc(len + 1);
    strcpy(stpcpy(&act[0], "sink:"), sink_name);
    INT_EVdfg_add_action(stone, &act[0]);
}

EVdfg_stone
INT_EVdfg_create_sink_stone(EVdfg dfg, char *sink_name)
{
    int len = strlen(sink_name) + strlen("sink:");
    char *act = malloc(len + 1);
    strcpy(stpcpy(&act[0], "sink:"), sink_name);
    return INT_EVdfg_create_stone(dfg, &act[0]);
}

void
INT_EVdfg_add_action(EVdfg_stone stone, char *action)
{
    if (stone->action == NULL) {
	stone->action = action;
	return;
    }
    if (stone->extra_actions == NULL) {
	stone->extra_actions = malloc(sizeof(stone->extra_actions[0]));
    } else {
	stone->extra_actions = realloc(stone->extra_actions,
				       stone->action_count * sizeof(stone->extra_actions[0]));
    }
    stone->extra_actions[stone->action_count - 1] = action;
    stone->action_count++;
}

EVdfg_stone
INT_EVdfg_create_stone(EVdfg dfg, char *action)
{
    EVdfg_stone stone = malloc(sizeof(struct _EVdfg_stone));
    stone->dfg = dfg;
    stone->node = -1;
    stone->bridge_stone = 0;
    stone->stone_id = -1;
    stone->attrs = NULL;
    stone->period_secs = -1;
    stone->period_usecs = -1;
    stone->out_count = 0;
    stone->out_links = NULL;
    stone->action_count = 1;
    stone->action = action;
    stone->extra_actions = NULL;
    stone->new_out_count = 0;
    stone->invalid = 0;
    stone->frozen = 0;
    stone->bridge_target = NULL;
    stone->pending_events = NULL;
    stone->processed_pending_events = NULL;
	
    if (dfg->stone_count == 0) {
	dfg->stones = malloc(sizeof(dfg->stones[0]));
    } else {
	dfg->stones = realloc(dfg->stones, 
			      sizeof(dfg->stones[0]) * (dfg->stone_count + 1));
    }
    stone->stone_id = 0x80000000 | dfg->stone_count;
    dfg->stones[dfg->stone_count++] = stone;
    return stone;
}

extern void 
INT_EVdfg_enable_auto_stone(EVdfg_stone stone, int period_sec, 
			int period_usec)
{
    stone->period_secs = period_sec;
    stone->period_usecs = period_usec;
}


static void fdump_dfg_stone(FILE* out, EVdfg_stone s);

static void 
reconfig_link_port(EVdfg_stone src, int port, EVdfg_stone dest, EVevent_list q_events)
{
    if (src->new_out_count == 0) {
        src->new_out_links = malloc(sizeof(src->new_out_links[0]));
        memset(src->new_out_links, 0, sizeof(src->new_out_links[0]));
        src->new_out_count = 1;
	src->new_out_ports = malloc(sizeof(src->new_out_ports[0]));
    } else {
        src->new_out_links = realloc(src->new_out_links,
				     sizeof(src->new_out_links[0]) * (src->new_out_count+1));
        memset(&src->new_out_links[src->new_out_count], 0, sizeof(src->new_out_links[0]));
        ++(src->new_out_count);
	src->new_out_ports = realloc(src->new_out_ports, sizeof(src->new_out_ports[0]) * (src->new_out_count));
    }
    src->new_out_links[src->new_out_count - 1] = dest;
    src->processed_pending_events = q_events;
    src->new_out_ports[src->new_out_count - 1] = port;
}


extern void INT_EVdfg_reconfig_link_port_to_stone(EVdfg dfg, int src_stone_index, int port, EVdfg_stone target_stone, EVevent_list q_events) {
	reconfig_link_port(dfg->stones[src_stone_index], port, target_stone, q_events);
}

extern void INT_EVdfg_reconfig_link_port_from_stone(EVdfg dfg, EVdfg_stone src_stone, int port, int target_index, EVevent_list q_events) {
	reconfig_link_port(src_stone, port, dfg->stones[target_index], q_events);
}

extern void INT_EVdfg_reconfig_link_port(EVdfg_stone src, int port, EVdfg_stone dest, EVevent_list q_events) {
	reconfig_link_port(src, port, dest, q_events);
}

extern void INT_EVdfg_reconfig_insert(EVdfg dfg, int src_stone_index, EVdfg_stone new_stone, int dest_stone_index, EVevent_list q_events) {
    reconfig_link_port(dfg->stones[src_stone_index], 0, new_stone, q_events);
    reconfig_link_port(new_stone, 0, dfg->stones[dest_stone_index], NULL);
    CMtrace_out(dfg->cm, EVdfgVerbose, "Inside reconfig_insert, sin = %d, min = %d, din = %d : \n", dfg->stones[src_stone_index]->node, new_stone->node, dfg->stones[dest_stone_index]->node);
}

extern void INT_EVdfg_reconfig_insert_on_port(EVdfg dfg, EVdfg_stone src_stone, int port, EVdfg_stone new_stone, EVevent_list q_events)
{
    EVdfg_stone dest_stone = src_stone->out_links[port];
    (void)dfg;
    reconfig_link_port(src_stone, port, new_stone, q_events);
    /* link port on the new stone to the old destination */
    reconfig_link_port(new_stone, port, dest_stone, NULL);
    CMtrace_out(dfg->cm, EVdfgVerbose, "Inside reconfig_insert_on_port, sin = %d, min = %d, din = %d : \n", src_stone->node, new_stone->node, dest_stone->node);
    printf("Inside reconfig_insert_on_port, sin = %x, min = %x, din = %x : \n", src_stone->stone_id, new_stone->stone_id, dest_stone->stone_id);
}

extern void
INT_EVdfg_link_port(EVdfg_stone src, int port, EVdfg_stone dest)
{
    if (port < 0) return;
    if (src->out_count == 0) {
	src->out_links = malloc(sizeof(src->out_links[0]) * (port+1));
	memset(src->out_links, 0, sizeof(src->out_links[0]) * (port+1));
	src->out_count = port + 1;
    } else if (src->out_count < port + 1) {
	src->out_links = realloc(src->out_links,
				 sizeof(src->out_links[0]) * (port+1));
	memset(&src->out_links[src->out_count], 0, sizeof(src->out_links[0]) * (port+1-src->out_count));
	src->out_count = port + 1;
    }
    src->out_links[port] = dest;
}

extern void
INT_EVdfg_set_attr_list(EVdfg_stone stone, attr_list attrs)
{
    if (stone->attrs != NULL) {
	fprintf(stderr, "Warning, attributes for stone %p previously set, overwriting\n", stone);
    }
    add_ref_attr_list(attrs);
    stone->attrs = attrs;
}

extern attr_list
INT_EVdfg_get_attr_list(EVdfg_stone stone)
{
    attr_list attrs = stone->attrs;
    if (attrs) {
	add_ref_attr_list(attrs);
    }
    return attrs;
}


static void check_all_nodes_registered(EVdfg_master master);
static void possibly_signal_shutdown(EVdfg_master master, int value, CMConnection conn);
static int new_shutdown_condition(EVdfg_client client, CMConnection conn);

static void
enable_auto_stones(CManager cm, EVdfg_client client)
{
    int i = 0;
    auto_stone_list *auto_list = client->pending_auto_list;
    client->pending_auto_list = NULL;
    CMtrace_out(cm, EVdfgVerbose, "ENABLING AUTO STONES, list is %p\n", auto_list);
    while (auto_list && auto_list[i].period_secs != -1) {
        /* everyone is ready, enable auto stones */
	CMtrace_out(cm, EVdfgVerbose, "auto stone %d, period %d sec, %d usec\n", auto_list[i].stone, auto_list[i].period_secs, auto_list[i].period_usecs);
	INT_EVenable_auto_stone(cm, auto_list[i].stone, auto_list[i].period_secs, auto_list[i].period_usecs);
	i++;
    }
    if (auto_list) free(auto_list);
}

static void
dfg_ready_handler(CManager cm, CMConnection conn, void *vmsg, 
		  void *client_data, attr_list attrs)
{
    EVdfg_client client = client_data;
    EVready_ptr msg =  vmsg;
    (void) conn;
    (void) attrs;
    client->my_node_id = msg->node_id;
    CManager_lock(cm);
    enable_auto_stones(cm, client);
    if (client->ready_condition != -1) {
	CMtrace_out(cm, EVdfgVerbose, "Client DFG %p Node id %d is ready, signalling %d\n", client, client->my_node_id, client->ready_condition);
	INT_CMCondition_signal(cm, client->ready_condition);
    } else {
	CMtrace_out(cm, EVdfgVerbose, "Client DFG %p Node id %d got ready, reconfig done\n", client, client->my_node_id);
    }	
    CManager_unlock(cm);
}

static void 
handle_conn_shutdown(EVdfg_master master, EVdfg_master_msg_ptr msg)
{
    int stone = msg->u.conn_shutdown.stone;
    EVdfg dfg = master->dfg;

    master->state = DFG_Reconfiguring;
    
    CMtrace_out(cm, EVdfgVerbose, "EVDFG conn_shutdown_handler -  master DFG state is now %s\n", str_state[master->state]);
    if (master->node_fail_handler != NULL) {
	int i;
	int target_stone = -1;
	char *failed_node = NULL;
	char *contact_str = NULL;
	CMtrace_out(cm, EVdfgVerbose, "IN CONN_SHUTDOWN_HANDLER\n");
	for (i=0; i< dfg->stone_count; i++) {
	    int j;
	    for (j = 0; j < dfg->stones[i]->out_count; j++) {
		if (dfg->stones[i]->out_links[j]->stone_id == stone) {
		    EVdfg_stone out_stone = dfg->stones[i]->out_links[j];
		    CMtrace_out(cm, EVdfgVerbose, "Found reporting stone as output %d of stone %d\n",
				j, i);
		    parse_bridge_action_spec(out_stone->action, 
					     &target_stone, &contact_str);
		    CMtrace_out(cm, EVdfgVerbose, "Dead stone is %d\n", target_stone);
		}
	    }
	}
	for (i=0; i< dfg->stone_count; i++) {
	    if (dfg->stones[i]->stone_id == target_stone) {
		int node = dfg->stones[i]->node;
		CMtrace_out(cm, EVdfgVerbose, "Dead node is %d, name %s\n", node,
			    master->nodes[node].canonical_name);
		failed_node = master->nodes[node].canonical_name;
		master->nodes[node].shutdown_status_contribution = STATUS_FAILED;
	    }
	}
	CManager_unlock(master->cm);
	master->node_fail_handler(dfg, failed_node, target_stone);
	CManager_lock(master->cm);
	master->reconfig = 1;
	master->sig_reconfig_bool = 1;
	check_all_nodes_registered(master);
    }
}

static void
dfg_shutdown_handler(CManager cm, CMConnection conn, void *vmsg, 
		  void *client_data, attr_list attrs)
{
    EVdfg_client client = client_data;
    EVshutdown_ptr msg =  vmsg;
    (void)cm;
    (void)conn;
    (void)attrs;
    int i = 0;
    CManager_lock(cm);
    /* I'm the client, all is done */
    client->shutdown_value = msg->value;
    client->already_shutdown = 1;
    CMtrace_out(cm, EVdfgVerbose, "Client %d has confirmed shutdown\n", client->my_node_id);
    while (client->shutdown_conditions && (client->shutdown_conditions[i] != -1)){
	CMtrace_out(cm, EVdfgVerbose, "Client %d shutdown signalling %d\n", client->my_node_id, client->shutdown_conditions[i]);
	INT_CMCondition_signal(client->cm, client->shutdown_conditions[i++]);
    }
//GSE  - does client have state?
//    CMtrace_out(cm, EVdfgVerbose, "EVDFG exit shutdown master DFG state is %s\n", str_state[client->state]);
    CManager_unlock(cm);
}

static void
handle_shutdown_contrib(EVdfg_master master, EVdfg_master_msg_ptr mmsg)
{

    EVshutdown_contribution_ptr msg =  &mmsg->u.shutdown_contrib;
    possibly_signal_shutdown(master, msg->value, mmsg->conn);
    CMtrace_out(cm, EVdfgVerbose, "EVDFG exit shutdown master DFG state is %s\n", str_state[master->state]);
}

static void
dfg_stone_close_handler(CManager cm, CMConnection conn, int stone, 
		  void *client_data)
{
    EVdfg_client client = (EVdfg_client)client_data;
    event_path_data evp = cm->evp;
    int global_stone_id = -1;
    CMFormat conn_shutdown_msg = INT_CMlookup_format(client->cm, EVdfg_conn_shutdown_format_list);
    EVconn_shutdown_msg msg;
    (void)cm;
    (void)conn;
    CManager_lock(cm);
    /* first, freeze the stone so that we don't lose any more data */
    INT_EVfreeze_stone(cm, stone);

    int i;
    for (i=0; i < evp->stone_lookup_table_size; i++ ) {
	if (stone == evp->stone_lookup_table[i].local_id) {
	    global_stone_id = evp->stone_lookup_table[i].global_id;
	}
    }
    if (global_stone_id == -1) {
	CMtrace_out(cm, EVdfgVerbose, "Bad mojo, failed to find global stone id after stone close of stone %d\n", stone);
	CMtrace_out(cm, EVdfgVerbose, "  If the above message occurs during shutdown, this is likely not a concern\n");
	CManager_unlock(cm);
	return;
    }
    msg.stone = global_stone_id;
    if (client->master_connection != NULL) {
	INT_CMwrite(client->master_connection, conn_shutdown_msg, &msg);
    } else {
	queue_master_msg(client->master, (void*)&msg, DFGconn_shutdown, NULL, /*copy*/0);
    }
    CManager_unlock(client->cm);
}

extern void
INT_EVdfg_assign_canonical_name(EVdfg_master master, char *given_name, char *canonical_name)
{
    int node;
    for (node = 0; node < master->node_count; node++) {
	if (master->nodes[node].name == given_name) {
	    if (master->dfg && (master->dfg->realized == 1)) {
		CMtrace_out(master->cm, EVdfgVerbose, "Reconfigure canonical name assignment, node = %d\n", node);
	    } else {
		CMtrace_out(master->cm, EVdfgVerbose, "Canonical name assignment, node = %d, given name was %s, canonical is %s\n", node, given_name, canonical_name);
	    }
	    master->nodes[node].canonical_name = strdup(canonical_name);
	}
    }
}

static void
handle_flush_reconfig(EVdfg_master master, EVdfg_master_msg_ptr mmsg)
{
    EVflush_attrs_reconfig_ptr msg = &mmsg->u.flush_reconfig;
    int i, j;
    EVdfg dfg = master->dfg;
    assert(CManager_locked(master->cm));
    if (((EVflush_attrs_reconfig_ptr)msg)->reconfig) {
	master->state = DFG_Reconfiguring;
    }
    CMtrace_out(cm, EVdfgVerbose, "EVDFG flush_attr_reconfig -  master DFG state is now %s\n", str_state[master->state]);
    for (i=0; i < msg->count; i++) {
	/* go through incoming attributes */
	for (j=0; j< dfg->stone_count; j++) {
	    if (dfg->stones[j]->stone_id == msg->attr_stone_list[i].stone) {
		if (dfg->stones[j]->attrs != NULL) {
		    free_attr_list(dfg->stones[j]->attrs);
		}
		dfg->stones[j]->attrs = attr_list_from_string(msg->attr_stone_list[i].attr_str);
		break;
	    }
	}
    }
    if (msg->reconfig) {
	CManager_unlock(master->cm);
	master->node_reconfig_handler(master->dfg);
	CManager_lock(master->cm);
	master->reconfig = 1;
	master->sig_reconfig_bool = 1;
	check_all_nodes_registered(master);
    }
}

static void
handle_node_join(EVdfg_master master, EVdfg_master_msg_ptr msg)
{
    char *node_name = msg->u.node_join.node_name;
    char *contact_string = msg->u.node_join.contact_string;
    CMConnection conn = msg->conn;
    int node;
    int new_node = -1;

    assert(CManager_locked(master->cm));

    if (master->state == DFG_Running) {
	master->state = DFG_Reconfiguring;
	CMtrace_out(cm, EVdfgVerbose, "EVDFG node_join -  master DFG state is now %s\n", str_state[master->state]);
    }

    if (master->node_join_handler == NULL) {
	/* static node list */
	for (node = 0; node < master->node_count; node++) {
	    if (strcmp(master->nodes[node].name, node_name) == 0) {
		if (conn == NULL) {
		    /* we are the master joining as a client node */
		    master->nodes[node].self = 1;
		    master->client->my_node_id = node;
		} else {
		    master->nodes[node].conn = conn;
		    master->nodes[node].str_contact_list = strdup(contact_string);
		    master->nodes[node].contact_list = attr_list_from_string(master->nodes[node].str_contact_list);
		    master->nodes[node].shutdown_status_contribution = STATUS_UNDETERMINED;
		}
		new_node = node;
		break;
	    }
	}
	if (new_node == -1) {
	    printf("Registering node \"%s\" not found in node list\n", 
		   node_name);
	    return;
	}
    } else {
	int n;
	
	if (master->dfg && master->dfg->realized == 1 && master->reconfig == 0) {
	    master->reconfig = 1;
	    master->sig_reconfig_bool = 1;
	    master->old_node_count = master->node_count;
	    CMtrace_out(master->cm, EVdfgVerbose, "Reconfigure, contact_string = %s\n", contact_string);
	    CMtrace_out(master->cm, EVdfgVerbose, "node_count = %d, stone_count = %d\n", master->node_count, master->dfg->stone_count);
	}
	n = master->node_count++;
	master->nodes = realloc(master->nodes, (sizeof(master->nodes[0])*master->node_count));
	memset(&master->nodes[n], 0, sizeof(master->nodes[0]));
	master->nodes[n].name = strdup(node_name);
	master->nodes[n].canonical_name = NULL;
	master->nodes[n].shutdown_status_contribution = STATUS_UNDETERMINED;
	new_node = n;
	if (conn == NULL) {
	    master->nodes[n].self = 1;
	    master->client->my_node_id = n;
	} else {
	    master->nodes[n].self = 0;
	    master->nodes[n].conn = conn;
	    master->nodes[n].str_contact_list = strdup(contact_string);
	    master->nodes[n].contact_list = attr_list_from_string(master->nodes[n].str_contact_list);
	}
    }
    CMtrace_out(cm, EVdfgVerbose, "Client \"%s\" has joined DFG, contact %s\n", node_name, master->nodes[new_node].str_contact_list);
    check_all_nodes_registered(master);
}


static void
dfg_deploy_handler(CManager cm, CMConnection conn, void *vmsg, 
		  void *client_data, attr_list attrs)
{
    EVdfg_client client = (EVdfg_client) client_data;
    event_path_data evp = cm->evp;
    (void) conn;
    (void) attrs;
    static int first_time_deploy = 1;
    EVdfg_deploy_ptr msg =  vmsg;
    int i, base = evp->stone_lookup_table_size;
    int auto_stones = 0;
    auto_stone_list *auto_list = malloc(sizeof(auto_stone_list));

    CMtrace_out(cm, EVdfgVerbose, "Client %d getting Deploy message\n", client->my_node_id);

    CManager_lock(cm);
    /* add stones to local lookup table */
    if (evp->stone_lookup_table_size == 0) {
	evp->stone_lookup_table = 
	    malloc(sizeof(evp->stone_lookup_table[0]) * msg->stone_count);
    } else {
	evp->stone_lookup_table = 
	    realloc(evp->stone_lookup_table,
		    sizeof(evp->stone_lookup_table[0]) * (msg->stone_count+base));
    }
    for (i=0; i < msg->stone_count; i++) {
	evp->stone_lookup_table[base + i].global_id = msg->stone_list[i].global_stone_id;
	evp->stone_lookup_table[base + i].local_id = INT_EValloc_stone(cm);
    }
    evp->stone_lookup_table_size = base + i;
    for (i=0; i < msg->stone_count; i++) {
	int local_stone = evp->stone_lookup_table[base + i].local_id;
	int local_list[1024]; /* better be enough */
	int j;
	if (msg->stone_list[i].attrs != NULL) {
	    attr_list tmp_attrs = attr_list_from_string(msg->stone_list[i].attrs);
	    INT_EVset_attr_list(cm, local_stone, tmp_attrs);
	    free_attr_list(tmp_attrs);
	}
	for (j=0; j < msg->stone_list[i].out_count; j++) {
	    if (msg->stone_list[i].out_links[j] != -1) {
		local_list[j] = lookup_local_stone(evp, msg->stone_list[i].out_links[j]);
		if (local_list[j] == -1) {
		    printf("Didn't found global stone %d\n", msg->stone_list[i].out_links[j]);
		}
	    } else {
		local_list[j] = -1;
	    }
	}
	local_list[msg->stone_list[i].out_count] = -1;
	INT_EVassoc_general_action(cm, local_stone, msg->stone_list[i].action, 
				   &local_list[0]);
	for (j=0; j < msg->stone_list[i].extra_actions; j++) {
	    INT_EVassoc_general_action(cm, local_stone, msg->stone_list[i].xactions[j], 
				       &local_list[0]);
	}	    
	if (msg->stone_list[i].period_secs != -1) {
	    auto_list= realloc(auto_list, sizeof(auto_list[0]) * (auto_stones+2));
	    auto_list[auto_stones].stone = local_stone;
	    auto_list[auto_stones].period_secs = msg->stone_list[i].period_secs;
	    auto_list[auto_stones].period_usecs = msg->stone_list[i].period_usecs;
	    auto_stones++;
	}
	if (action_type(msg->stone_list[i].action) == Action_Terminal) {
	    client->active_sink_count++;
	}
    }    
    auto_list[auto_stones].period_secs = -1;
    if (conn != NULL) {
	CMFormat deploy_ack_msg = INT_CMlookup_format(client->cm, EVdfg_deploy_ack_format_list);
	EVdeploy_ack_msg response_msg;
	response_msg.node_id = msg->canonical_name;
	INT_CMwrite(client->master_connection, deploy_ack_msg, &response_msg);
	CMtrace_out(cm, EVdfgVerbose, "Client %d wrote deploy ack\n", client->my_node_id);
    } else {
      	CMtrace_out(cm, EVdfgVerbose, "Client %d no master conn\n", client->my_node_id);
    }
    if (first_time_deploy) {
	first_time_deploy = 0;
    }
    client->pending_auto_list = auto_list;
    
    CManager_unlock(cm);
}

static void
free_dfg(CManager cm, void *vdfg)
{
    EVdfg dfg = vdfg;
    int i;
    /* for (i=0; i < dfg->node_count; i++) { */
    /* 	free(dfg->nodes[i].name); */
    /* 	free(dfg->nodes[i].canonical_name); */
    /* 	if (dfg->nodes[i].str_contact_list) free(dfg->nodes[i].str_contact_list); */
    /* 	if (dfg->nodes[i].contact_list) free_attr_list(dfg->nodes[i].contact_list); */
    /* } */
    /* free(dfg->nodes); */
    for (i=0; i < dfg->stone_count; i++) {
	if (dfg->stones[i]->out_links) free(dfg->stones[i]->out_links);
	if (dfg->stones[i]->action) free(dfg->stones[i]->action);
	if (dfg->stones[i]->extra_actions) {
	    int j;
	    for (j=0; j < dfg->stones[i]->action_count-1; j++) {
		free(dfg->stones[i]->extra_actions[j]);
	    }
	    free(dfg->stones[i]->extra_actions);
	}
	if (dfg->stones[i]->attrs) free_attr_list(dfg->stones[i]->attrs);
	free(dfg->stones[i]);
    }
    /* while (dfg->queued_messages) { */
    /* 	EVdfg_master_msg_ptr tmp = dfg->queued_messages->next; */
    /* 	free_master_msg(dfg->queued_messages); */
    /* 	dfg->queued_messages = tmp; */
    /* } */
    /* if (dfg->master_contact_str) free(dfg->master_contact_str); */
    /* if (dfg->shutdown_conditions) free(dfg->shutdown_conditions); */
    free(dfg->stones);
    free(dfg);
}

static EVflush_attrs_reconfig_ptr
build_attrs_msg(EVdfg_client client)
{
    CManager cm = client->cm;
    event_path_data evp = cm->evp;
    int i = 0, cur_stone;
    EVflush_attrs_reconfig_ptr msg = malloc(sizeof(*msg));
    memset(msg, 0, sizeof(*msg));
    msg->attr_stone_list = malloc(sizeof(EVattr_stone_struct));
    for (cur_stone = evp->stone_base_num; cur_stone < evp->stone_count + evp->stone_base_num; ++cur_stone) {
	stone_type stone = stone_struct(evp, cur_stone);
	if (stone->stone_attrs != NULL) {
	    msg->attr_stone_list[i].stone = lookup_global_stone(evp, stone->local_id);
	    msg->attr_stone_list[i].attr_str = attr_list_to_string(stone->stone_attrs);
	    i++;
	    msg->attr_stone_list = realloc(msg->attr_stone_list, sizeof(EVattr_stone_struct)*(i+1));
	}
    }
    msg->count = i;
    return (msg);
}

static void
free_attrs_msg(EVflush_attrs_reconfig_ptr msg)
{
    int i = 0;
    for (i = 0; i < msg->count; i++) {
	free(msg->attr_stone_list[i].attr_str);
    }
    free(msg->attr_stone_list);
    free(msg);
}

static void
flush_and_trigger(EVdfg_client client, int reconfig)
{
    EVflush_attrs_reconfig_ptr msg = build_attrs_msg(client);
    CMFormat flush_msg = INT_CMlookup_format(client->cm, EVdfg_flush_attrs_reconfig_format_list);
    msg->reconfig = reconfig;
    if (client->master_connection != NULL) {
	/* we are a client, send the reconfig to the master */
	INT_CMwrite(client->master_connection, flush_msg, msg);
	free_attrs_msg(msg);
    } else {
	queue_master_msg(client->master, &flush_msg, DFGflush_reconfig, NULL, /*copy*/ 0);
    }
}


static void
cod_EVdfg_trigger_reconfig(cod_exec_context ec)
{
    CManager cm = get_cm_from_ev_state((void*)cod_get_client_data(ec, 0x34567890));
    event_path_data evp = cm->evp;
    EVdfg_client client = evp->app_stone_close_data;  /* cheating a bit.  We know we store the DFG pointer here */
    flush_and_trigger(client, 1);
}

static void
cod_EVdfg_flush_attrs(cod_exec_context ec)
{
    CManager cm = get_cm_from_ev_state((void*)cod_get_client_data(ec, 0x34567890));
    event_path_data evp = cm->evp;
    EVdfg_client client = evp->app_stone_close_data;  /* cheating a bit.  We know we store the DFG pointer here */
    flush_and_trigger(client, 0);
}

extern EVdfg_master
INT_EVdfg_create_master(CManager cm)
{
    EVdfg_master master = malloc(sizeof(struct _EVdfg_master));
    attr_list contact_list;

    memset(master, 0, sizeof(struct _EVdfg_master));
    master->cm = cm;
    master->reconfig = 0;
    master->sig_reconfig_bool = 0;
    master->old_node_count = 1;
    master->no_deployment = 0;
    master->state = DFG_Joining;

    CMtrace_out(cm, EVdfgVerbose, "EVDFG initialization -  master DFG state set to %s\n", str_state[master->state]);
    contact_list = INT_CMget_contact_list(cm);
    master->my_contact_str = attr_list_to_string(contact_list);
    free_attr_list(contact_list);

    /*
     * EVdfg master-sent messages
     */
    INT_CMregister_format(cm, EVdfg_ready_format_list);
    INT_CMregister_format(cm, EVdfg_deploy_format_list);
    INT_CMregister_format(cm, EVdfg_shutdown_format_list);

    /*
     * EVdfg master-handled messages
     */
    INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_node_join_format_list),
			   dfg_master_msg_handler, (void*)(((uintptr_t)master)|DFGnode_join));
    INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_deploy_ack_format_list),
			   dfg_master_msg_handler, (void*)(((uintptr_t)master)|DFGdeploy_ack));
    INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_shutdown_contribution_format_list),
			   dfg_master_msg_handler, (void*)(((uintptr_t)master)|DFGshutdown_contrib));
    INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_conn_shutdown_format_list),
			   dfg_master_msg_handler, (void*)(((uintptr_t)master)|DFGconn_shutdown));
    INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_flush_attrs_reconfig_format_list),
			   dfg_master_msg_handler, (void*)(((uintptr_t)master)|DFGflush_reconfig));

    INT_CMadd_poll(cm, handle_queued_messages_lock, master);
    return master;
}

extern EVdfg
INT_EVdfg_create(EVdfg_master master)
{
    EVdfg dfg = malloc(sizeof(struct _EVdfg));
    attr_list contact_list;

    memset(dfg, 0, sizeof(struct _EVdfg));
    dfg->master = master;
    dfg->deployed_stone_count = 0;
    dfg->deploy_ack_condition = -1;
    master->dfg = dfg;
    if (master->client) {
	master->client->dfg = dfg;
	dfg->client = master->client;
    }
    master->reconfig = 0;
    master->sig_reconfig_bool = 0;
    master->old_node_count = 1;
    master->state = DFG_Joining;
    CMtrace_out(cm, EVdfgVerbose, "EVDFG initialization -  master DFG state set to %s\n", str_state[master->state]);

    INT_CMadd_shutdown_task(master->cm, free_dfg, dfg, FREE_TASK);
    return dfg;
}


extern char *INT_EVdfg_get_contact_list(EVdfg_master master)
{
    attr_list listen_list, contact_list = NULL;
    atom_t CM_TRANSPORT = attr_atom_from_string("CM_TRANSPORT");
    CManager cm = master->cm;

    /* use enet transport if available */
    listen_list = create_attr_list();
    add_string_attr(listen_list, CM_TRANSPORT, strdup("enet"));
    contact_list = INT_CMget_specific_contact_list(cm, listen_list);

    free_attr_list(listen_list);
    if (contact_list == NULL) {
	contact_list = INT_CMget_contact_list(cm);
	if (contact_list == NULL) {
	    CMlisten(cm);
	    contact_list = INT_CMget_contact_list(cm);
	}
    }
    master->master_command_contact_str = attr_list_to_string(contact_list);
    free_attr_list(contact_list);
    return master->master_command_contact_str;
}

static void
check_connectivity(EVdfg dfg)
{
    int i;
    for (i=0; i< dfg->stone_count; i++) {
	CMtrace_out(dfg->cm, EVdfgVerbose, "Stone %d - assigned to node %s, action %s\n", i, 
		    dfg->master->nodes[dfg->stones[i]->node].canonical_name, (dfg->stones[i]->action ? dfg->stones[i]->action : "NULL"));
	if (dfg->stones[i]->node == -1) {
	    printf("Warning, stone %d has not been assigned to any node.  This stone will not be deployed.\n", i);
	    printf("    This stones particulars are:\n");
	    fdump_dfg_stone(stdout, dfg->stones[i]);
	}
	if (dfg->stones[i]->action_count == 0) {
	    printf("Warning, stone %d (assigned to node %s) has no actions registered", i, dfg->master->nodes[dfg->stones[i]->node].canonical_name);
	    continue;
	}
	if ((dfg->stones[i]->out_count == 0) && (dfg->stones[i]->new_out_count == 0)) {
	    char *action_spec = dfg->stones[i]->action;
	    switch(action_type(action_spec)) {
	    case Action_Terminal:
	    case Action_Bridge:
		break;
	    default:
		printf("Warning, stone %d (assigned to node %s) has no outputs connected to other stones\n", i, dfg->master->nodes[dfg->stones[i]->node].canonical_name);
		printf("    This stones particulars are:\n");
		fdump_dfg_stone(stdout, dfg->stones[i]);
		break;
	    }
	}
    }
}

extern int
INT_EVdfg_realize(EVdfg dfg)
{
    check_connectivity(dfg);
//    check_types(dfg);

    if (dfg->realized == 1) {
	dfg->master->reconfig = 0;
    }
    dfg->realized = 1;
    return 1;
}

extern void
INT_EVdfg_register_node_list(EVdfg_master master, char **nodes)
{
    int count = 0, i = 0;
    while(nodes[count] != NULL) count++;
    master->node_count = count;
    master->nodes = malloc(sizeof(master->nodes[0]) * count);
    memset(master->nodes, 0, sizeof(master->nodes[0]) * count);
    for (i = 0; i < master->node_count; i++) {
	master->nodes[i].name = strdup(nodes[i]);
	master->nodes[i].canonical_name = strdup(nodes[i]);
	master->nodes[i].shutdown_status_contribution = STATUS_UNDETERMINED;
    }
}

extern void
INT_EVdfg_assign_node(EVdfg_stone stone, char *node_name)
{
    EVdfg dfg = stone->dfg;
    EVdfg_master master = dfg->master;
    int i, node = -1;
    for (i = 0; i < master->node_count; i++) {
	EVint_node_list n = &master->nodes[i];
	if (n->canonical_name && (strcmp(n->canonical_name, node_name) == 0)) {
	    node = i;
	} else 	if (n->name && (strcmp(n->name, node_name) == 0)) {
	    node = i;
	}

    }
    if (node == -1) {
	printf("Node \"%s\" not found in node list\n", node_name);
    }
	
    if (dfg->realized == 1) {
	CMtrace_out(dfg->cm, EVdfgVerbose, "assign node, node# = %d\n", node);
    }
    stone->node = node;
}

extern int 
INT_EVdfg_ready_wait(EVdfg_client client)
{
    CMtrace_out(cm, EVdfgVerbose, "DFG %p wait for ready\n", client);
    INT_CMCondition_wait(client->cm, client->ready_condition);
    client->ready_condition = -1;
    CMtrace_out(cm, EVdfgVerbose, "DFG %p ready wait released\n", client);
    return 1;
}

extern int
INT_EVdfg_shutdown(EVdfg_client client, int result)
{
    CMFormat shutdown_msg = INT_CMlookup_format(client->cm, EVdfg_shutdown_contribution_format_list);
    EVshutdown_contribution_msg msg;
    if (client->already_shutdown) printf("Node %d, already shut down BAD!\n", client->my_node_id);
    msg.value = result;
    if (client->master_connection == NULL) {
	queue_master_msg(client->master, (void*)&msg, DFGshutdown_contrib, NULL, /*copy*/0);
    } else {
	/* we are a client, tell the master to shutdown */
	INT_CMwrite(client->master_connection, shutdown_msg, &msg);
    }
    if (!client->already_shutdown) {
	CManager_unlock(client->cm);
	CMCondition_wait(client->cm, new_shutdown_condition(client, client->master_connection));
	CManager_lock(client->cm);
    }
    return client->shutdown_value;
}

extern int
INT_EVdfg_force_shutdown(EVdfg_client client, int result)
{
    result |= STATUS_FORCE;
    if (client->already_shutdown) printf("Node %d, already contributed to shutdown.  Don't call shutdown twice!\n", client->my_node_id);
    if (client->master_connection != NULL) {
	/* we are a client, tell the master to shutdown */
	CMFormat shutdown_msg = INT_CMlookup_format(client->cm, EVdfg_shutdown_contribution_format_list);
	EVshutdown_contribution_msg msg;
	msg.value = result;
	INT_CMwrite(client->master_connection, shutdown_msg, &msg);
	/* and wait until we hear back */
    } else {
	possibly_signal_shutdown(client->master, result, NULL);
    }
    if (!client->already_shutdown) {
	CManager_unlock(client->cm);
	CMCondition_wait(client->cm, new_shutdown_condition(client, client->master_connection));
	CManager_lock(client->cm);
    }
    return client->shutdown_value;
}

extern int
INT_EVdfg_active_sink_count(EVdfg_client client)
{
    return client->active_sink_count;
}

extern void
INT_EVdfg_ready_for_shutdown(EVdfg_client client)
{
    if (client->already_shutdown) return;
    if (client->master_connection != NULL) {
	/* we are a client, tell the master to shutdown */
	CMFormat shutdown_msg = INT_CMlookup_format(client->cm, EVdfg_shutdown_contribution_format_list);
	EVshutdown_contribution_msg msg;
	msg.value = STATUS_NO_CONTRIBUTION;   /* no status contribution */
	INT_CMwrite(client->master_connection, shutdown_msg, &msg);
    } else {
	possibly_signal_shutdown(client->master, STATUS_NO_CONTRIBUTION, NULL);
    }
}

extern int 
INT_EVdfg_wait_for_shutdown(EVdfg_client client)
{
//    printf("Wait for shutdown called for node %d\n", dfg->my_node_id);
    if (client->already_shutdown) return client->shutdown_value;
//    printf("Wait for shutdown waiting for node %d\n", dfg->my_node_id);
    INT_CMCondition_wait(client->cm, new_shutdown_condition(client, client->master_connection));
//    printf("Wait for shutdown returning for node %d\n", dfg->my_node_id);
    return client->shutdown_value;
}

extern int INT_EVdfg_source_active(EVsource src)
{
    return (src->local_stone_id != -1);
}

extern void
INT_EVdfg_register_source(char *name, EVsource src)
{
    CManager cm = src->cm;
    event_path_data evp = cm->evp;
    if (evp->source_count == 0) {
	evp->sources = malloc(sizeof(evp->sources[0]));
    } else {
	evp->sources = realloc(evp->sources,
			       sizeof(evp->sources[0]) * (evp->source_count + 1));
    }
    evp->sources[evp->source_count].name = name;
    evp->sources[evp->source_count].src = src;
    evp->source_count++;
}

extern void
INT_EVdfg_register_sink_handler(CManager cm, char *name, FMStructDescList list, EVSimpleHandlerFunc handler, void* client_data)
{
    event_path_data evp = cm->evp;
    if (evp->sink_handler_count == 0) {
	evp->sink_handlers = malloc(sizeof(evp->sink_handlers[0]));
    } else {
	evp->sink_handlers = realloc(evp->sink_handlers,
				     sizeof(evp->sink_handlers[0]) * (evp->sink_handler_count + 1));
    }
    evp->sink_handlers[evp->sink_handler_count].name = name;
    evp->sink_handlers[evp->sink_handler_count].format_list = list;
    evp->sink_handlers[evp->sink_handler_count].handler = handler;
    evp->sink_handlers[evp->sink_handler_count].client_data = client_data;
    evp->sink_handler_count++;
}

extern void
INT_EVdfg_register_raw_sink_handler(CManager cm, char *name, EVRawHandlerFunc handler)
{
    event_path_data evp = cm->evp;
    if (evp->sink_handler_count == 0) {
	evp->sink_handlers = malloc(sizeof(evp->sink_handlers[0]));
    } else {
	evp->sink_handlers = realloc(evp->sink_handlers,
				     sizeof(evp->sink_handlers[0]) * (evp->sink_handler_count + 1));
    }
    evp->sink_handlers[evp->sink_handler_count].name = name;
    evp->sink_handlers[evp->sink_handler_count].format_list = NULL;
    evp->sink_handlers[evp->sink_handler_count].handler = (EVSimpleHandlerFunc)handler;
    evp->sink_handler_count++;
}

static int
new_shutdown_condition(EVdfg_client client, CMConnection conn)
{
    int cur_count = 0;
    if (client->shutdown_conditions == NULL) {
	client->shutdown_conditions = malloc(2*sizeof(client->shutdown_conditions[0]));
    } else {
	while (client->shutdown_conditions[cur_count++] != -1) ; 
	cur_count--;
	client->shutdown_conditions = realloc(client->shutdown_conditions, 
					   (cur_count+2)*sizeof(client->shutdown_conditions[0]));
    }
    client->shutdown_conditions[cur_count] = INT_CMCondition_get(client->cm, conn);
    client->shutdown_conditions[cur_count+1] = -1;
    return client->shutdown_conditions[cur_count];
}

static char dfg_extern_string[] = "\
	void EVdfg_trigger_reconfiguration(cod_exec_context ec);\n\
	void EVdfg_flush_attrs(cod_exec_context ec);\n\
";

static cod_extern_entry dfg_extern_map[] = {
    {"EVdfg_trigger_reconfiguration", (void *) 0},
    {"EVdfg_flush_attrs", (void *) 0},
    {(void*)0, (void*)0}
};

extern EVdfg_client
dfg_assoc_client(CManager cm, char* node_name, char *master_contact, EVdfg_master master)
{
    event_path_data evp = cm->evp;
    attr_list master_attrs = NULL;
    CMConnection conn;
    CMFormat register_msg = NULL;
    EVnode_join_msg msg;
    attr_list contact_list = INT_CMget_contact_list(cm);
    char *my_contact_str;
    EVdfg_client client;
    int i;


    dfg_extern_map[0].extern_value = (void*)(long)cod_EVdfg_trigger_reconfig;
    dfg_extern_map[1].extern_value = (void*)(long)cod_EVdfg_flush_attrs;

    INT_EVadd_standard_routines(cm, dfg_extern_string, dfg_extern_map);

    client = malloc(sizeof(*client));
    memset(client, 0, sizeof(*client));
    client->cm = cm;
    client->pending_auto_list = NULL;
    if (master_contact) {
	master_attrs = attr_list_from_string(master_contact);
	client->master_contact_str = strdup(master_contact);
    } else {
	client->master = master;
	client->dfg = master->dfg;
	if (master->dfg)
	    master->dfg->client = client;
	master->client = client;
    }

    client->ready_condition = INT_CMCondition_get(cm, NULL);

    if (contact_list == NULL) {
	INT_CMlisten(cm);
	contact_list = INT_CMget_contact_list(cm);
    }

    my_contact_str = attr_list_to_string(contact_list);
    free_attr_list(contact_list);
    
    msg.node_name = strdup(node_name);
    msg.contact_string = my_contact_str;
    msg.source_count = evp->source_count;
    msg.sources = malloc(msg.source_count * sizeof(msg.sources[0]));
    for (i=0; i < evp->source_count; i++) {
	msg.sources[i].name = strdup(evp->sources[i].name);
	msg.sources[i].FMtype = NULL;
    }
    msg.sink_count = evp->sink_handler_count;
    msg.sinks = malloc(msg.sink_count * sizeof(msg.sinks[0]));
    for (i=0; i < evp->sink_handler_count; i++) {
	msg.sinks[i].name = strdup(evp->sink_handlers[i].name);
	msg.sinks[i].FMtype = NULL;
    }
    INT_EVregister_close_handler(cm, dfg_stone_close_handler, (void*)client);
    
    if (master) {
	/* local master */
	queue_master_msg(master, (void*)&msg, DFGnode_join, NULL, /*copy*/0);
    } else {
	/*
	 * EVdfg client-sent messages
	 */
	register_msg = INT_CMregister_format(cm, EVdfg_node_join_format_list);
	INT_CMregister_format(cm, EVdfg_deploy_ack_format_list);
	INT_CMregister_format(cm, EVdfg_shutdown_contribution_format_list);
	INT_CMregister_format(cm, EVdfg_conn_shutdown_format_list);
	INT_CMregister_format(cm, EVdfg_flush_attrs_reconfig_format_list);

	/*
	 * EVdfg client-handled messages
	 */
	INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_ready_format_list),
			       dfg_ready_handler, client);
	INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_deploy_format_list),
			       dfg_deploy_handler, client);
	INT_CMregister_handler(INT_CMregister_format(cm, EVdfg_shutdown_format_list),
			       dfg_shutdown_handler, client);

	conn = INT_CMget_conn(cm, master_attrs);
	if (conn == NULL) {
	    fprintf(stderr, "failed to contact Master at %s\n", attr_list_to_string(master_attrs));
	    fprintf(stderr, "Join DFG failed\n");
	    return NULL;
	}
	INT_CMwrite(conn, register_msg, &msg);
	client->master_connection = conn;
	for (i=0; i < evp->source_count; i++) {
	    free(msg.sources[i].name);
	}
	free(msg.sources);
	for (i=0; i < evp->sink_handler_count; i++) {
	    free(msg.sinks[i].name);
	}
	free(msg.sinks);
	free(msg.contact_string);
	free(msg.node_name);
    }
    CMtrace_out(cm, EVdfgVerbose, "DFG %p node name %s\n", client, node_name);
    if (master_attrs) free_attr_list(master_attrs);
    return client;
}

extern EVdfg_client
INT_EVdfg_assoc_client_local(CManager cm, char* node_name, EVdfg_master master)
{
    return dfg_assoc_client(cm, node_name, NULL, master);
}

extern EVdfg_client
INT_EVdfg_assoc_client(CManager cm, char* node_name, char *master_contact_str)
{
    return dfg_assoc_client(cm, node_name, master_contact_str, NULL);
}

static EVdfg_stone
create_bridge_stone(EVdfg dfg, EVdfg_stone target)
{
    EVdfg_stone stone = NULL;
    char *contact = dfg->master->nodes[target->node].str_contact_list;
    char *action;
    if (dfg->master->nodes[target->node].self) {
	contact = dfg->master->my_contact_str;
    }
    action = INT_create_bridge_action_spec(target->stone_id, contact);
    stone = INT_EVdfg_create_stone(dfg, action);
    stone->bridge_stone = 1;
    stone->bridge_target = target;
	
    return stone;
}

static void
add_bridge_stones(EVdfg dfg)
{
    int i;
    for (i=0; i< dfg->stone_count; i++) {
	int j;
	for (j = 0; j < dfg->stones[i]->out_count; j++) {
	    EVdfg_stone cur = dfg->stones[i];
	    EVdfg_stone target = cur->out_links[j];
	    if (target && (!cur->bridge_stone) && (cur->node != target->node)) {
		cur->out_links[j] = create_bridge_stone(dfg, target);
		/* put the bridge stone where the source stone is */
		cur->out_links[j]->node = cur->node;
		CMtrace_out(dfg->cm, EVdfgVerbose, "Created bridge stone %p, target node is %d, assigned to node %d\n", cur->out_links[j], target->node, cur->node);
	    }
	}
    }
}

static void
assign_stone_ids(EVdfg dfg)
{
    int i;
    for (i=0; i< dfg->stone_count; i++) {
	dfg->stones[i]->stone_id = 0x80000000 | i;
    }
}

static void
deploy_to_node(EVdfg dfg, int node)
{
    int i, j;
    int stone_count = 0;
    EVdfg_deploy_msg msg;
    CMFormat deploy_msg = INT_CMlookup_format(dfg->master->cm, EVdfg_deploy_format_list);

    for (i=dfg->deployed_stone_count; i< dfg->stone_count; i++) {
	if (dfg->stones[i]->node == node) {
	    stone_count++;
	}
    }
    CMtrace_out(cm, EVdfgVerbose, "Master in deploy_to_node for client %s, node %d, stones to deploy %d\n",
		dfg->master->nodes[node].canonical_name, node, stone_count);
    if (stone_count == 0) {
        dfg->deploy_ack_count++;
	dfg->master->nodes[node].needs_ready = 0;
      	return;
    }
    memset(&msg, 0, sizeof(msg));
    msg.canonical_name = dfg->master->nodes[node].canonical_name;
    msg.stone_count = stone_count;
    msg.stone_list = malloc(stone_count * sizeof(msg.stone_list[0]));
    memset(msg.stone_list, 0, stone_count * sizeof(msg.stone_list[0]));
    j = 0;
    for (i=dfg->deployed_stone_count; i< dfg->stone_count; i++) {
	if (dfg->stones[i]->node == node) {
	    deploy_msg_stone mstone = &msg.stone_list[j];
	    EVdfg_stone dstone = dfg->stones[i];
	    int k;
	    mstone->global_stone_id = dstone->stone_id;
	    mstone->attrs = NULL;
	    if (dstone->attrs != NULL) {
		mstone->attrs = attr_list_to_string(dstone->attrs);
	    }
	    mstone->period_secs = dstone->period_secs;
	    mstone->period_usecs = dstone->period_usecs;
	    mstone->out_count = dstone->out_count;
	    mstone->out_links = malloc(sizeof(mstone->out_links[0])*mstone->out_count);
	    for (k=0; k< dstone->out_count; k++) {
		if (dstone->out_links[k] != NULL) {
		    mstone->out_links[k] = dstone->out_links[k]->stone_id;
		} else {
		    mstone->out_links[k] = -1;
		}
	    }
	    mstone->action = dstone->action;
	    if (dstone->action_count > 1) {
		mstone->extra_actions = dstone->action_count - 1;
		mstone->xactions = malloc(sizeof(mstone->xactions[0])*mstone->extra_actions);
		for (k=0; k < mstone->extra_actions; k++) {
		    mstone->xactions[k] = dstone->extra_actions[k];
		}
	    } else {
		mstone->extra_actions = 0;
		mstone->xactions = NULL;
	    }
	    j++;
	}
    }
    dfg->master->nodes[node].needs_ready = 1;
    if (dfg->master->nodes[node].conn) {
	INT_CMwrite(dfg->master->nodes[node].conn, deploy_msg, &msg);
    } else {
	CManager_unlock(dfg->master->cm);
	dfg_deploy_handler(dfg->master->cm, NULL, &msg, dfg->master->client, NULL);
	CManager_lock(dfg->master->cm);
    }
    for(i=0 ; i < msg.stone_count; i++) {
	free(msg.stone_list[i].out_links);
	if (msg.stone_list[i].attrs) free(msg.stone_list[i].attrs);
	if (msg.stone_list[i].xactions) free(msg.stone_list[i].xactions);
    }
    free(msg.stone_list);
}


void reconfig_delete_link(EVdfg dfg, int src_index, int dest_index) {
    int i;
	
    EVdfg_stone src = dfg->stones[src_index];
    EVdfg_stone dest = dfg->stones[dest_index];
    EVdfg_stone temp_stone = NULL;
	
    for (i = 0; i < src->out_count; ++i) {
	if (src->bridge_stone == 0) {
	    if (src->out_links[i]->bridge_stone) {
		temp_stone = src->out_links[i];
		if (temp_stone->bridge_target == dest) {
		    if (src->node == 0) {
			//EVfreeze_stone(dfg->cm, temp_stone->stone_id);
			//transfer_events = EVextract_stone_events(dfg->cm, temp_stone->stone_id);
			if (src->frozen == 0) {
			    EVfreeze_stone(dfg->client->cm, src->stone_id);
			}
			EVstone_remove_split_target(dfg->client->cm, src->stone_id, temp_stone->stone_id);
			//	    EVfreeze_stone(dfg->cm, temp_stone->stone_id);
			//	    transfer_events = EVextract_stone_events(dfg->cm, temp_stone->stone_id);
			EVfree_stone(dfg->client->cm, temp_stone->stone_id);
		    } else {
			if (src->frozen == 0) {
			    REVfreeze_stone(dfg->master->nodes[src->node].conn, src->stone_id);
			}
			REVstone_remove_split_target(dfg->master->nodes[src->node].conn, src->stone_id, temp_stone->stone_id);
			//	    REVfreeze_stone(dfg->nodes[temp_stone->node].conn, temp_stone->stone_id);
			
			CMtrace_out(dfg->cm, EVdfgVerbose, "deleting remotely.. sounds good till here.. src->node = %d, src_index = %d\n", src->node, src_index);
			fflush(stdout);
			
			//	    transfer_events = REVextract_stone_events(dfg->nodes[temp_stone->node].conn, temp_stone->stone_id);
			
			CMtrace_out(dfg->cm, EVdfgVerbose, "exracted events in delete..\n");
			fflush(stdout);
			
			REVfree_stone(dfg->master->nodes[src->node].conn, temp_stone->stone_id);
			//free(temp-stone);
		    }
		    //temp_stone = NULL;
		    src->out_links[i]->invalid = 1;
		    src->frozen = 1;
		    break;
		}
	    } else {
		if(src->out_links[i] == dest && src->out_links[i]->invalid == 0) {
		    if (src->node == 0) {
			if (src->frozen == 0) {
			    EVfreeze_stone(dfg->client->cm, src->stone_id);
			}
			EVstone_remove_split_target(dfg->client->cm, src->stone_id, dest->stone_id);
		    } else {
			if (src->frozen == 0) {
			    REVfreeze_stone(dfg->master->nodes[src->node].conn, src->stone_id);
			}
			REVstone_remove_split_target(dfg->master->nodes[src->node].conn, src->stone_id, dest->stone_id);
		    }
		    src->out_links[i]->invalid = 1;
		    src->frozen = 1;
		    break;
		}
	    }
	}
    }
   
}


static void reconfig_deploy(EVdfg dfg) 
{
    int i;
    int j;
    EVstone new_bridge_stone_id = -1;
    EVdfg_stone temp_stone = NULL;
    EVdfg_stone cur = NULL;
    EVdfg_stone target = NULL;
	
	
    for (i = 0; i < dfg->master->node_count; ++i) {
	deploy_to_node(dfg, i);
    }
	
    CManager_unlock(dfg->master->cm);
    for (i = 0; i < dfg->stone_count; ++i) {
	if (dfg->stones[i]->new_out_count > 0) {
	    cur = dfg->stones[i];
	    CMtrace_out(dfg->cm, EVdfgVerbose, "Reconfig_deploy, stone %d, (%x)\n", i, cur->stone_id);
	    if (cur->frozen == 0) {
		CMtrace_out(dfg->cm, EVdfgVerbose, "Freezing stone %d, (%x)\n", i, cur->stone_id);
		if (cur->node == 0) {
		    /* Master */
		    EVfreeze_stone(dfg->master->cm, cur->stone_id);
		} else {
		    REVfreeze_stone(dfg->master->nodes[cur->node].conn, cur->stone_id);
		}
		cur->frozen = 1;
	    }
	    for (j = 0; j < cur->new_out_count; ++j) {
		CMtrace_out(dfg->cm, EVdfgVerbose, " stone %d (%x) has new out link\n", i, cur->stone_id);
		if (cur->new_out_ports[j] < cur->out_count) {
		    temp_stone = cur->out_links[cur->new_out_ports[j]];
		} else {
		    temp_stone = NULL;
		}
		INT_EVdfg_link_port(cur, cur->new_out_ports[j], cur->new_out_links[j]);
		
		target = cur->out_links[cur->new_out_ports[j]];
		if (target && (cur->bridge_stone == 0) && (cur->node != target->node)) {
		    cur->out_links[cur->new_out_ports[j]] = create_bridge_stone(dfg, target);
		    CMtrace_out(dfg->cm, EVdfgVerbose, "Created bridge stone (reconfig_deploy) %p, target node %d, assigned to node %d\n", cur->out_links[j], target->node, cur->node);
		    /* put the bridge stone where the source stone is */
		    cur->out_links[cur->new_out_ports[j]]->node = cur->node;
		    cur->out_links[cur->new_out_ports[j]]->pending_events = cur->processed_pending_events;
		    if (cur->node == 0) {
			new_bridge_stone_id = EVcreate_bridge_action(dfg->client->cm, dfg->master->nodes[target->node].contact_list, target->stone_id);
			cur->out_links[cur->new_out_ports[j]]->stone_id = new_bridge_stone_id;
			EVstone_add_split_target(dfg->client->cm, cur->stone_id, new_bridge_stone_id);
		    } else {
			new_bridge_stone_id = REVcreate_bridge_action(dfg->master->nodes[cur->node].conn, dfg->master->nodes[target->node].contact_list, target->stone_id);
			REVstone_add_split_target(dfg->master->nodes[cur->node].conn, cur->stone_id, new_bridge_stone_id);
		    }
		} else {
		    if (cur->node == 0) {
			EVstone_add_split_target(dfg->client->cm, cur->stone_id, target->stone_id);
		    }
		    else {
			REVstone_add_split_target(dfg->master->nodes[cur->node].conn, cur->stone_id, target->stone_id);
		    }
		}
		
		if (temp_stone != NULL) {
		    if (temp_stone->invalid == 0) {
			if (temp_stone->frozen == 0) {
			    if (temp_stone->node == 0) {
				EVfreeze_stone(dfg->client->cm, temp_stone->stone_id);
				EVtransfer_events(dfg->client->cm, temp_stone->stone_id, cur->out_links[cur->new_out_ports[j]]->stone_id);
			    } else {
				REVfreeze_stone(dfg->master->nodes[temp_stone->node].conn, temp_stone->stone_id);
				REVtransfer_events(dfg->master->nodes[temp_stone->node].conn, temp_stone->stone_id, cur->out_links[cur->new_out_ports[j]]->stone_id);
			    }
			}
			if (cur->node == 0) {
			    EVstone_remove_split_target(dfg->client->cm, cur->stone_id, temp_stone->stone_id);
			} else {
			    REVstone_remove_split_target(dfg->master->nodes[cur->node].conn, cur->stone_id, temp_stone->stone_id);
			}

			if (temp_stone->bridge_stone) {
			    if (cur->node == 0) {
				CMtrace_out(dfg->client->cm, EVdfgVerbose, "\nreconfig_deploy: Locally freeing..\n");
				fflush(stdout);
				EVfree_stone(dfg->client->cm, temp_stone->stone_id);
			    } else {
				CMtrace_out(dfg->cm, EVdfgVerbose, "reconfig_deploy: Remotely freeing..\n");
				fflush(stdout);
				REVfree_stone(dfg->master->nodes[temp_stone->node].conn, temp_stone->stone_id);
			    }
			    //free(temp_stone);
			    //temp_stone = NULL;
			}
			temp_stone->invalid = 1;
		    }
		}
	    }
	}
    }
    
    
    /* ****** Transferring events ******
     */
    
    for (i = 0; i < dfg->transfer_events_count; ++i) {
	EVdfg_stone temp = dfg->stones[dfg->transfer_events_list[i][0]];
	EVdfg_stone src = temp->out_links[dfg->transfer_events_list[i][1]];
	EVdfg_stone dest;
	
	CMtrace_out(dfg->cm, EVdfgVerbose, " Transfering events\n");
	if (temp->node == 0) {
	    if (temp->frozen == 0) {
		EVfreeze_stone(dfg->client->cm, temp->stone_id);
		temp->frozen = 1;
	    }
	    if (src->frozen == 0) {
		EVfreeze_stone(dfg->client->cm, src->stone_id);
		src->frozen = 1;
	    }
	} else {
	    if (temp->frozen == 0) {
		REVfreeze_stone(dfg->master->nodes[temp->node].conn, temp->stone_id);
		temp->frozen = 1;
	    }
	    if (src->frozen == 0) {
		REVfreeze_stone(dfg->master->nodes[src->node].conn, src->stone_id);
		src->frozen = 1;
	    }
	}
	
	temp = dfg->stones[dfg->transfer_events_list[i][2]];
	dest = temp->out_links[dfg->transfer_events_list[i][3]];
	
	if (src->node == 0) {
	    EVtransfer_events(dfg->client->cm, src->stone_id, dest->stone_id);
	} else {
	    REVtransfer_events(dfg->master->nodes[src->node].conn, src->stone_id, dest->stone_id);
	}
    }
    
    /* ****** Deleting links ******
     */
    
    for (i = 0; i < dfg->delete_count; ++i) {
	CMtrace_out(dfg->cm, EVdfgVerbose, " Deleting a link\n");
	reconfig_delete_link(dfg, dfg->delete_list[i][0], dfg->delete_list[i][1]);
    }
    
    for (i = 0; i < dfg->stone_count; ++i) {
        cur = dfg->stones[i];
	if (cur->frozen == 1 && cur->invalid == 0) {
	    if (dfg->stones[i]->new_out_count > 0) {
		free(cur->new_out_links);
		free(cur->new_out_ports);
		cur->new_out_count = 0;
	    }
	    if (cur->node == 0) {
		//	    if (cur->pending_events != NULL) {
		//	      printf("\nResubmitting events locally! Cheers!\n");
		//	      fflush(EVsubmit);
		//	      stdout_encoded(dfg->cm, cur->stone_id, cur->pending_events->buffer, cur->pending_events->length, dfg->nodes[0].contact_list);
		//	    }
		EVunfreeze_stone(dfg->client->cm, cur->stone_id);
	    } else {
		//	    if (cur->pending_events != NULL) {
		//	      printf("\nResubmitting events remotely! Cheers!\n");
		//	      fflush(stdout);
		//REVsubmit_encoded(dfg->nodes[cur->node].conn, cur->stone_id, cur->pending_events->buffer, cur->pending_events->length, dfg->nodes[cur->node].contact_list);
		//            }
		REVunfreeze_stone(dfg->master->nodes[cur->node].conn, cur->stone_id);
	    }
	    cur->frozen = 0;
	}
    }
    
    
    CManager_lock(dfg->client->cm);
    if (CMtrace_on(dfg->cm, EVdfgVerbose)) {
	for (i = 0; i < dfg->stone_count; ++i) {
	    fprintf(CMTrace_file, "Stone# %d : ", i);
	    fdump_dfg_stone(CMTrace_file, dfg->stones[i]);
	}
    }
    dfg->deployed_stone_count = dfg->stone_count;

}

static void
dump_dfg_stone(EVdfg_stone s)
{
    fdump_dfg_stone(stdout, s);
}

static void
fdump_dfg_stone(FILE* out, EVdfg_stone s)
{
    int i;

    (void)dump_dfg_stone;   /* stop warning aboud dump_dfg_stone, handy to keep around for debugging */

    fprintf(out, "stone %p, node %d, stone_id %x\n", s, s->node, s->stone_id);
    if (s->bridge_stone) fprintf(out, "      bridge_stone\n");
    fprintf(out, " out_count %d : ", s->out_count);
    for (i=0; i < s->out_count; i++) {
	fprintf(out, "%p, ", s->out_links[i]);
    }
    fprintf(out, "\n action_count %d, action = \"%s\"\n", s->action_count, (s->action ? s->action : "NULL"));
    fprintf(out, "new_out_count %d : ", s->new_out_count);
    for (i=0; i < s->new_out_count; i++) {
	fprintf(out, "(port %d) -> %p, ", s->new_out_ports[i], s->new_out_links[i]);
    }
    fprintf(out, "\nbridge_target %p\n", s->bridge_target);
}

static void
free_master_msg(EVdfg_master_msg *msg)
{
    switch(msg->msg_type) {
    case DFGnode_join: {
	EVnode_join_ptr in = &msg->u.node_join;
	int i;
	free(in->node_name);
	free(in->contact_string);
	for (i=0; i < in->sink_count; i++) {
	    leaf_element *l = &in->sinks[i];
	    if(l->name) free(l->name);
	    if(l->FMtype) free(l->FMtype);
	}
	free(in->sinks);
	for (i=0; i < in->source_count; i++) {
	    leaf_element *l = &in->sources[i];
	    if (l->name) free(l->name);
	    if (l->FMtype) free(l->FMtype);
	}
	free(in->sources);
	break;
    }
    case DFGflush_reconfig: {
	EVflush_attrs_reconfig_ptr in = &msg->u.flush_reconfig;
	int i;
	for (i=0 ; i < in->count; i++) {
	    free(in->attr_stone_list[i].attr_str);
	}
	free(in->attr_stone_list);
	break;
    }
    case DFGdeploy_ack:
    case DFGshutdown_contrib:
    case DFGconn_shutdown:
    default:
	break;
    }
    free(msg);
}

static void
queue_master_msg(EVdfg_master master, void*vmsg, EVmaster_msg_type msg_type, CMConnection conn, int copy)
{
    EVdfg_master_msg_ptr msg = malloc(sizeof(EVdfg_master_msg));
    msg->msg_type = msg_type;
    msg->conn = conn;
    switch(msg_type) {
    case DFGnode_join: {
	EVnode_join_ptr in = (EVnode_join_ptr)vmsg;
	if (!copy) {
	    msg->u.node_join = *in;
	} else {
	    int i;
	    msg->u.node_join.node_name = strdup(in->node_name);
	    msg->u.node_join.contact_string = strdup(in->contact_string);
	    msg->u.node_join.source_count = in->source_count;
	    msg->u.node_join.sink_count = in->sink_count;
	    msg->u.node_join.sinks = (leaf_element*)malloc(sizeof(leaf_element) * in->sink_count);
	    for (i=0; i < in->sink_count; i++) {
		leaf_element *l = &in->sinks[i];
		msg->u.node_join.sinks[i].name = l->name ? strdup(l->name) : NULL;
		msg->u.node_join.sinks[i].FMtype = l->FMtype ? strdup(l->FMtype) : NULL;
	    }
	    msg->u.node_join.sources = (leaf_element*)malloc(sizeof(leaf_element) * in->source_count);
	    for (i=0; i < in->source_count; i++) {
		leaf_element *l = &in->sources[i];
		msg->u.node_join.sources[i].name = l->name ? strdup(l->name) : NULL;
		msg->u.node_join.sources[i].FMtype = l->FMtype ? strdup(l->FMtype) : NULL;
	    }
	}
	break;
    }
    case DFGdeploy_ack: {
	EVdeploy_ack_ptr in = (EVdeploy_ack_ptr)vmsg;
	msg->u.deploy_ack = *in;
	break;
    }
    case DFGshutdown_contrib: {
	EVshutdown_contribution_ptr in = (EVshutdown_contribution_ptr)vmsg;
	msg->u.shutdown_contrib = *in;
	break;
    }
    case  DFGconn_shutdown: {
	EVconn_shutdown_ptr in = (EVconn_shutdown_ptr)vmsg;
	msg->u.conn_shutdown = *in;
	break;
    }
    case DFGflush_reconfig: {
	EVflush_attrs_reconfig_ptr in = (EVflush_attrs_reconfig_ptr)vmsg;
	msg->u.flush_reconfig = *in;
	if (copy) {
	    int i;
	    msg->u.flush_reconfig.attr_stone_list = malloc(sizeof(EVattr_stone_struct) * in->count);
	    for (i=0 ; i < in->count; i++) {
		msg->u.flush_reconfig.attr_stone_list[i].stone = in->attr_stone_list[i].stone;
		msg->u.flush_reconfig.attr_stone_list[i].attr_str = strdup(in->attr_stone_list[i].attr_str);
	    }
	}
	break;
    }
    default:
	assert(FALSE);
    }
    msg->next = NULL;
    if (master->queued_messages == NULL) {
	master->queued_messages = msg;
    } else {
	EVdfg_master_msg_ptr last = master->queued_messages;
	while (last->next != NULL) last = last->next;
	last->next = msg;
    }
    if (master->cm->control_list->server_thread != 0) {
	CMwake_server_thread(master->cm);
    } else {
	handle_queued_messages(master->cm, master);
    }
}

static void
dfg_master_msg_handler(CManager cm, CMConnection conn, void *vmsg, 
		       void *client_data, attr_list attrs)
{
    EVdfg_master master = (EVdfg_master)((uintptr_t)client_data & (~0xf));
    EVmaster_msg_type msg_type = ((uintptr_t)client_data & 0xf);
    queue_master_msg(master, vmsg, msg_type, conn, /*copy*/1);
    /* we'll handle this in the poll handler */
}

static void
handle_deploy_ack(EVdfg_master master, EVdfg_master_msg_ptr mmsg)
{
    EVdeploy_ack_ptr msg =  &mmsg->u.deploy_ack;
    CManager cm = master->cm;
    EVdfg dfg = master->dfg;
    master->dfg->deploy_ack_count++;
    CMtrace_out(cm, EVdfgVerbose, "Client %s reports deployed, count %d\n", msg->node_id, master->dfg->deploy_ack_count);
    if ((master->dfg->deploy_ack_count == dfg->master->node_count) && (dfg->deploy_ack_condition != -1)) {
	CMtrace_out(cm, EVdfgVerbose, "That was the last one, Signalling %d\n", dfg->deploy_ack_condition);
	CMtrace_out(cm, EVdfgVerbose, "EVDFG exit deploy ack handler -  master DFG state is %s\n", str_state[master->state]);
	INT_CMCondition_signal(cm, master->dfg->deploy_ack_condition);
	master->dfg->deploy_ack_condition = -1;
	assert(master->state == DFG_Starting);
	master->state = DFG_Running;
	CMtrace_out(cm, EVdfgVerbose, "EVDFG  -  master DFG state set to %s\n", str_state[master->state]);
    } else {
      if (master->state == DFG_Reconfiguring) {
	master->state = DFG_Running;
	CMtrace_out(cm, EVdfgVerbose, "EVDFG after reconfiguration -  master DFG state set to %s\n", str_state[master->state]);
      }
    }
    CMtrace_out(cm, EVdfgVerbose, "EVDFG exit deploy ack handler -  master DFG state is %s\n", str_state[master->state]);
}

extern void INT_EVdfg_reconfig_transfer_events(EVdfg dfg, int src_stone_index, int src_port, int dest_stone_index, int dest_port) 
{
	
    if (dfg->transfer_events_count == 0) {
	dfg->transfer_events_list = malloc(sizeof(int *));
    } else {
	dfg->transfer_events_list = realloc(dfg->transfer_events_list, (dfg->transfer_events_count + 1) * sizeof(int *));
    }
	
    dfg->transfer_events_list[dfg->transfer_events_count] = malloc(4 * sizeof(int));
	
    dfg->transfer_events_list[dfg->transfer_events_count][0] = src_stone_index;
    dfg->transfer_events_list[dfg->transfer_events_count][1] = src_port;
    dfg->transfer_events_list[dfg->transfer_events_count][2] = dest_stone_index;
    dfg->transfer_events_list[dfg->transfer_events_count][3] = dest_port;
	
    ++dfg->transfer_events_count;
}

static void reconfig_add_bridge_stones(EVdfg dfg) 
{	
    int i;
    int j;
    int k;
    EVdfg_stone cur = NULL;
	
    for (i = dfg->deployed_stone_count; i < dfg->stone_count; ++i) {
	if (dfg->stones[i]->bridge_stone == 0) {
	    cur = dfg->stones[i];
	    for (k = 0; k < dfg->master->node_count; ++k) {
		if (k == cur->node && cur->new_out_count !=0 ) {
		    for (j = 0; j < cur->new_out_count; ++j) {
			INT_EVdfg_link_port(cur, cur->new_out_ports[j], cur->new_out_links[j]);
		    }
		    
		    free(cur->new_out_links);
		    free(cur->new_out_ports);
		    
		    cur->new_out_links = NULL;
		    cur->new_out_ports = NULL;
		    
		    cur->new_out_count = 0;
		    for (j = 0; j < cur->out_count; ++j) {
			EVdfg_stone temp_stone, target = cur->out_links[j];
			if (target->bridge_stone) {
			    temp_stone = target;
			    target = target->bridge_target;
			}
			if (target && (!cur->bridge_stone) && (cur->node != target->node)) {
			    cur->out_links[j] = create_bridge_stone(dfg, target);
			    /* put the bridge stone where the source stone is */
			    cur->out_links[j]->node = cur->node;
			    printf("Built bridge stone %x\n", cur->out_links[j]->stone_id);
			}
		    }
		    break;
		}
	    }
	}
    }
}


extern void INT_EVdfg_reconfig_delete_link(EVdfg dfg, int src_index, int dest_index)
{
    if (dfg->delete_count == 0) {
	dfg->delete_list = malloc(sizeof(int *));
    } else {
	dfg->delete_list = realloc(dfg->delete_list, (dfg->delete_count + 1) * sizeof(int *));
    }
	
    dfg->delete_list[dfg->delete_count] = malloc(2 * sizeof(int));
	
    dfg->delete_list[dfg->delete_count][0] = src_index;
    dfg->delete_list[dfg->delete_count][1] = dest_index;
	
    ++dfg->delete_count;
}


extern
void INT_REVdfg_freeze_next_bridge_stone(EVdfg dfg, int stone_index)
{
    REVfreeze_stone(dfg->master->nodes[dfg->stones[stone_index]->node].conn, dfg->stones[stone_index]->out_links[0]->stone_id);

    if (dfg->realized == 1) {
	dfg->master->reconfig = 0;
    }
    dfg->master->no_deployment = 1;
}

extern
void INT_EVdfg_freeze_next_bridge_stone(EVdfg dfg, int stone_index) 
{
    EVfreeze_stone(dfg->client->cm, dfg->stones[stone_index]->out_links[0]->stone_id);
	
    if (dfg->realized == 1) {
	dfg->master->reconfig = 0;
    }
    dfg->master->no_deployment = 1;
}

static void
perform_deployment(EVdfg dfg)
{
    int i;
    EVdfg_master master = dfg->master;

    if (dfg->master->sig_reconfig_bool == 0) {
	assert(master->state == DFG_Joining);
	master->state = DFG_Starting;
	CMtrace_out(cm, EVdfgVerbose, "EVDFG check all nodes registered -  master DFG state is %s\n", str_state[master->state]);
	assign_stone_ids(dfg);
	add_bridge_stones(dfg);
	dfg->deploy_ack_count = 1;  /* we are number 1 */
	if (dfg->deploy_ack_condition == -1) {
	    dfg->deploy_ack_condition = INT_CMCondition_get(dfg->client->cm, NULL);
	}
	for (i=0; i < dfg->master->node_count; i++) {
	    deploy_to_node(dfg, i);
	    dfg->master->nodes[i].needs_ready = 1;   /* everyone needs a ready the first time through */
	}
    } else {
        CMtrace_out(cm, EVdfgVerbose, "EVDFG perform_deployment -  master DFG state set to %s\n", str_state[master->state]);
	assert(master->state == DFG_Reconfiguring);
	reconfig_add_bridge_stones(dfg);
	reconfig_deploy(dfg);
    }
}

static void
wait_for_deploy_acks(EVdfg dfg)
{
    if (dfg->deploy_ack_count != dfg->master->node_count) {
	if (dfg->deploy_ack_condition != -1)  {
	    CManager_unlock(dfg->master->cm);
	    CMCondition_wait(dfg->master->cm, dfg->deploy_ack_condition);
	    CManager_lock(dfg->master->cm);
	}
    }
}

static void
reconfig_signal_ready(EVdfg dfg)
{
    int i;
    CMFormat ready_msg = INT_CMlookup_format(dfg->master->cm, EVdfg_ready_format_list);
    EVready_msg msg;
    CMtrace_out(cm, EVdfgVerbose, "Master signaling DFG %p ready for operation\n",
				dfg);
    for (i=dfg->master->old_node_count; i < dfg->master->node_count; i++) {
	if (dfg->master->nodes[i].conn != NULL) {
	    msg.node_id = i;
	    INT_CMwrite(dfg->master->nodes[i].conn, ready_msg, &msg);
	    CMtrace_out(cm, EVdfgVerbose, "Master - ready sent to node \"%s\"\n",
			dfg->master->nodes[i].name);
	}
    }
}

static void
signal_ready(EVdfg dfg)
{
    int i;
    CMFormat ready_msg = INT_CMlookup_format(dfg->master->cm, EVdfg_ready_format_list);
    EVready_msg msg;
    CMtrace_out(cm, EVdfgVerbose, "Master signaling DFG %p ready for operation\n",
		dfg);
    for (i=0; i < dfg->master->node_count; i++) {
	if (!dfg->master->nodes[i].needs_ready) {
	    CMtrace_out(cm, EVdfgVerbose, "Master - ready not required for node %d \"%s\"\n", i, 
			dfg->master->nodes[i].name);
	    continue;
	}
	if (dfg->master->nodes[i].conn != NULL) {
	    msg.node_id = i;
	    INT_CMwrite(dfg->master->nodes[i].conn, ready_msg, &msg);
	    CMtrace_out(cm, EVdfgVerbose, "Master - ready sent to node %d \"%s\"\n", i, 
			dfg->master->nodes[i].name);
	} else {
	    if (!dfg->master->nodes[i].self) {
		printf("Failure, no connection, not self, node %d\n", i);
		exit(1);
	    }
	    CManager_unlock(dfg->client->cm);
	    msg.node_id = i;
	    CMtrace_out(cm, EVdfgVerbose, "Master DFG %p is ready, local signalling %d\n", dfg, dfg->client->ready_condition);
	    dfg_ready_handler(dfg->client->cm, NULL, &msg, dfg->client, NULL);
	    CManager_lock(dfg->client->cm);
	}
	dfg->master->nodes[i].needs_ready = 0;
    }
}

static void
possibly_signal_shutdown(EVdfg_master master, int value, CMConnection conn)
{
    int i;
    CMFormat shutdown_msg = INT_CMlookup_format(master->cm, EVdfg_shutdown_format_list);
    EVshutdown_msg msg;
    int status = STATUS_SUCCESS;
    int shutdown = 1;
    int force_shutdown = 0;
    int signal_from_client = -1;
    assert(CManager_locked(master->cm));
    for (i=0; i < master->node_count; i++) {
	if ((conn == NULL) && master->nodes[i].self) {
	    /* we're the master and node i */
	    signal_from_client = i;
	} else if (conn == master->nodes[i].conn) {
	    signal_from_client = i;
	}
    }
	
    if ((value >= 0) && ((value & STATUS_FORCE) == STATUS_FORCE)) {
	force_shutdown = 1;
	value ^= STATUS_FORCE;   /* yes, that's xor-assign */
    }
    if (force_shutdown) {
	CMtrace_out(cm, EVdfgVerbose, "Client %d signals %d, forces shutdown\n", signal_from_client, value);
    } else {
	CMtrace_out(cm, EVdfgVerbose, "Client %d signals %d, See if we're all ready to signal shutdown\n", signal_from_client, value);
    }
    master->nodes[signal_from_client].shutdown_status_contribution = value;
    int contributed_status = 0;
    for (i=0; i < master->node_count; i++) {
	CMtrace_out(cm, EVdfgVerbose, "NODE %d status is :", i);
	switch (master->nodes[i].shutdown_status_contribution) {
	case STATUS_UNDETERMINED:
	    CMtrace_out(cm, EVdfgVerbose, "NOT READY FOR SHUTDOWN\n");
	    shutdown = 0;
	    break;
	case STATUS_NO_CONTRIBUTION:
	    CMtrace_out(cm, EVdfgVerbose, "READY for shutdown, no status\n");
	    break;
	case STATUS_SUCCESS:
	    CMtrace_out(cm, EVdfgVerbose, "READY for shutdown, SUCCESS\n");
	    contributed_status = 1;
	    break;
	case STATUS_FAILED:
	    CMtrace_out(cm, EVdfgVerbose, "ALREADY FAILED\n");
	    contributed_status = 1;
	    break;
	default:
	    CMtrace_out(cm, EVdfgVerbose, "READY for shutdown, FAILURE %d\n",
			master->nodes[i].shutdown_status_contribution);
	    status |= master->nodes[i].shutdown_status_contribution;
	    break;
	}
    }
    if (force_shutdown) {
	shutdown = 1;
	status = value;
	CMtrace_out(cm, EVdfgVerbose, "DFG undergoing forced shutdown\n");
    }
    if (!shutdown) {
	CMtrace_out(cm, EVdfgVerbose, "DFG not ready for shutdown\n");
	return;
    }
    if (!contributed_status) {
	CMtrace_out(cm, EVdfgVerbose, "DFG nobody has contributed status - not ready for shutdown\n");
	return;
    }
    CMtrace_out(cm, EVdfgVerbose, "DFG shutdown with value %d\n", status);
    master->state = DFG_Shutting_Down;
    CMtrace_out(cm, EVdfgVerbose, "EVDFG possibly signal shutdown -  master DFG state is %s\n", str_state[master->state]);
    msg.value = status;
    for (i=0; i < master->node_count; i++) {
	if (master->nodes[i].conn != NULL) {
	    INT_CMwrite(master->nodes[i].conn, shutdown_msg, &msg);
	    CMtrace_out(cm, EVdfgVerbose, "DFG shutdown message sent to client \"%s\"(%d)\n", master->nodes[i].canonical_name, i);
	} else {
	    if (!master->nodes[i].self) {
		printf("Failure, no connection, not self, node %d\n", i);
		exit(1);
	    }
	}
    }
    master->client->shutdown_value = status;
    i = 0;
    master->client->already_shutdown = 1;
    while(master->client->shutdown_conditions && (master->client->shutdown_conditions[i] != -1)) {
	CMtrace_out(cm, EVdfgVerbose, "Client %d shutdown signalling %d\n", master->client->my_node_id, master->client->shutdown_conditions[i]);
	INT_CMCondition_signal(master->cm, master->client->shutdown_conditions[i++]);
    }
    CMtrace_out(cm, EVdfgVerbose, "Master DFG shutdown\n");
}

extern void INT_EVdfg_node_join_handler(EVdfg_master master, EVdfgJoinHandlerFunc func)
{
    master->node_join_handler = func;
}

extern void INT_EVdfg_node_fail_handler(EVdfg_master master, EVdfgFailHandlerFunc func)
{
    master->node_fail_handler = func;
}

extern void INT_EVdfg_node_reconfig_handler(EVdfg_master master, EVdfgReconfigHandlerFunc func)
{
    master->node_reconfig_handler = func;
}

static void
check_all_nodes_registered(EVdfg_master master)
{
    int i;
    EVdfg dfg = master->dfg;
    if (master->node_join_handler != NULL) {
	EVint_node_list node = &master->nodes[master->node_count-1];
	CManager_unlock(master->cm);
	(master->node_join_handler)(master, node->name, NULL, NULL);
	CManager_lock(master->cm);
	dfg = master->dfg;
	if ((dfg == NULL) || (dfg->realized == 0) || 
	    (dfg->realized == 1 && master->reconfig == 1)) return;
    } else {
	/* must be static node list */
	for(i=0; i<master->node_count; i++) {
	    if (!master->nodes[i].self && (master->nodes[i].conn == NULL)) {
		return;
	    }
	}
    }
	
    if (master->no_deployment == 0) {
	perform_deployment(dfg);
	wait_for_deploy_acks(dfg);
    }
    master->no_deployment = 0;
    signal_ready(dfg);
    dfg->deployed_stone_count = dfg->stone_count;
    master->old_node_count = master->node_count;
}

static FMField EVleaf_element_flds[] = {
    {"name", "string", sizeof(char*), FMOffset(leaf_element*, name)},
    {"FMtype", "string", sizeof(char*), FMOffset(leaf_element*, FMtype)},
    {NULL, NULL, 0, 0}
};

static FMField EVnode_join_msg_flds[] = {
    {"node_name", "string", sizeof(char*), FMOffset(EVnode_join_ptr, node_name)},
    {"contact_string", "string", sizeof(char*), FMOffset(EVnode_join_ptr, contact_string)},
    {"source_count", "integer", sizeof(int), FMOffset(EVnode_join_ptr, source_count)},
    {"sink_count", "integer", sizeof(int), FMOffset(EVnode_join_ptr, sink_count)},
    {"sources", "source_element[source_count]", sizeof(leaf_element), FMOffset(EVnode_join_ptr, sources)},
    {"sinks", "sink_element[sink_count]", sizeof(leaf_element), FMOffset(EVnode_join_ptr, sinks)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_node_join_format_list[] = {
    {"EVdfg_node_join", EVnode_join_msg_flds, sizeof(EVnode_join_msg), NULL},
    {"sink_element", EVleaf_element_flds, sizeof(leaf_element), NULL},
    {"source_element", EVleaf_element_flds, sizeof(leaf_element), NULL},
    {NULL, NULL, 0, NULL}
};

static FMField EVready_msg_flds[] = {
    {"node_id", "integer", sizeof(int), FMOffset(EVready_ptr, node_id)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_ready_format_list[] = {
    {"EVdfg_ready", EVready_msg_flds, sizeof(EVready_msg), NULL},
    {NULL, NULL, 0, NULL}
};

static FMField EVdeploy_ack_msg_flds[] = {
    {"node_id", "string", sizeof(char*), FMOffset(EVdeploy_ack_ptr, node_id)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_deploy_ack_format_list[] = {
    {"EVdfg_deploy_ack", EVdeploy_ack_msg_flds, sizeof(EVdeploy_ack_msg), NULL},
    {NULL, NULL, 0, NULL}
};

static FMField EVshutdown_msg_flds[] = {
    {"value", "integer", sizeof(int), FMOffset(EVshutdown_ptr, value)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_shutdown_format_list[] = {
    {"EVdfg_shutdown", EVshutdown_msg_flds, sizeof(EVshutdown_msg), NULL},
    {NULL, NULL, 0, NULL}
};

static FMField EVshutdown_contribution_msg_flds[] = {
    {"value", "integer", sizeof(int), FMOffset(EVshutdown_contribution_ptr, value)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_shutdown_contribution_format_list[] = {
    {"EVdfg_shutdown_contribution", EVshutdown_contribution_msg_flds, sizeof(EVshutdown_contribution_msg), NULL},
    {NULL, NULL, 0, NULL}
};

static FMField EVconn_shutdown_msg_flds[] = {
    {"stone", "integer", sizeof(int), FMOffset(EVconn_shutdown_ptr, stone)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_conn_shutdown_format_list[] = {
    {"EVdfg_conn_shutdown", EVconn_shutdown_msg_flds, sizeof(EVconn_shutdown_msg), NULL},
    {NULL, NULL, 0, NULL}
};

static FMField EVattr_stone_flds[] = {
    {"stone", "integer", sizeof(long), FMOffset(EVattr_stone_ptr, stone)},
    {"attr_str", "string", sizeof(char*), FMOffset(EVattr_stone_ptr, attr_str)},
    {NULL, NULL, 0, 0}
};

static FMField EVflush_attrs_reconfig_msg_flds[] = {
    {"reconfig", "integer", sizeof(int), FMOffset(EVflush_attrs_reconfig_ptr, reconfig)},
    {"count", "integer", sizeof(long), FMOffset(EVflush_attrs_reconfig_ptr, count)},
    {"attr_stone_list", "attr_stone_element[count]", sizeof(EVattr_stone_struct), FMOffset(EVflush_attrs_reconfig_ptr, attr_stone_list)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_flush_attrs_reconfig_format_list[] = {
    {"EVflush_attrs_reconfig", EVflush_attrs_reconfig_msg_flds, sizeof(EVflush_attrs_reconfig_msg), NULL},
    {"attr_stone_element", EVattr_stone_flds, sizeof(EVattr_stone_struct), NULL},
    {NULL, NULL, 0, NULL}
};

static FMField EVdfg_stone_flds[] = {
    {"global_stone_id", "integer", sizeof(int), 
     FMOffset(deploy_msg_stone, global_stone_id)},
    {"attrs", "string", sizeof(char*), 
     FMOffset(deploy_msg_stone, attrs)},
    {"period_secs", "integer", sizeof(int),
     FMOffset(deploy_msg_stone, period_secs)},
    {"period_usecs", "integer", sizeof(int),
     FMOffset(deploy_msg_stone, period_usecs)},
    {"out_count", "integer", sizeof(int), 
     FMOffset(deploy_msg_stone, out_count)},
    {"out_links", "integer[out_count]", sizeof(int), 
     FMOffset(deploy_msg_stone, out_links)},
    {"action", "string", sizeof(char*), 
     FMOffset(deploy_msg_stone, action)},
    {"extra_actions", "integer", sizeof(int), 
     FMOffset(deploy_msg_stone, extra_actions)},
    {"xactions", "string[extra_actions]", sizeof(char*), 
     FMOffset(deploy_msg_stone, xactions)},
    {NULL, NULL, 0, 0}
};

static FMField EVdfg_deploy_msg_flds[] = {
    {"canonical_name", "string", sizeof(char*),
     FMOffset(EVdfg_deploy_ptr, canonical_name)},
    {"stone_count", "integer", sizeof(int),
     FMOffset(EVdfg_deploy_ptr, stone_count)},
    {"stone_list", "EVdfg_deploy_stone[stone_count]", sizeof(struct _EVdfg_msg_stone), FMOffset(EVdfg_deploy_ptr, stone_list)},
    {NULL, NULL, 0, 0}
};

static FMStructDescRec EVdfg_deploy_format_list[] = {
    {"EVdfg_deploy", EVdfg_deploy_msg_flds, sizeof(EVdfg_deploy_msg), NULL},
    {"EVdfg_deploy_stone", EVdfg_stone_flds, sizeof(struct _EVdfg_msg_stone), NULL},
    {NULL, NULL, 0, NULL}
};
