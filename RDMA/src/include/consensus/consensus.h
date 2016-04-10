#ifndef CONSENSUS_H

#define CONSENSUS_H
#include "../util/common-header.h"

#include "../output/output.h"

#define CSM      1
#define OUTPUT   2

typedef uint64_t db_key_type;
struct node_t;
struct consensus_component_t;

typedef void (*user_cb)(size_t data_size,void* data,void* arg);

typedef enum con_role_t{
    LEADER = 0,
    SECONDARY = 1,
}con_role;

struct consensus_component_t* init_consensus_comp(struct node_t*,int,uint32_t,FILE*,int,int,
        const char*,void*,int,
        view*,view_stamp*,view_stamp*,view_stamp*,user_cb,void*);

int consensus_submit_request(struct consensus_component_t*,size_t,void*,output_peer_t*);

void *handle_accept_req(void* arg);

#endif