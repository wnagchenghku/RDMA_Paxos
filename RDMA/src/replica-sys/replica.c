#include "../include/proxy/proxy.h"
#include "../include/util/common-header.h"
#include "../include/replica-sys/node.h"
#include "../include/config-comp/config-comp.h"
#include "../include/rdma/rdma_common.h"
#include <zookeeper.h>

#include <sys/stat.h>

static zhandle_t *zh;
static int is_connected;

struct znodes_data
{
    uint32_t node_id;
    uint32_t tail;
};

struct watcherContext
{
    uint32_t node_id;
    pthread_mutex_t *lock;
    char znode_path[64];
    view* cur_view;
};

#define ZDATALEN 1024 * 1024

static void get_znode_path(const char *pathbuf, char *znode_path)
{
    const char *p = pathbuf;
    int i;
    for (i = strlen(pathbuf); i >= 0; i--)
    {
        if (*(p + i) == '/')
        {
            break;
        }
    }
    strcpy(znode_path, "/election/");
    strcat(znode_path, p + i + 1);
}

void zookeeper_init_watcher(zhandle_t *izh, int type, int state, const char *path, void *context)
{
    if (type == ZOO_SESSION_EVENT)
    {
        if (state == ZOO_CONNECTED_STATE)
        {
            is_connected = 1;
        } else if (state == ZOO_EXPIRED_SESSION_STATE) {
            is_connected = 0;
            zookeeper_close(izh);
        }
    }
}

static int check_leader(view* cur_view, uint32_t node_id, char *znode_path)
{
    int rc, i, zoo_data_len = ZDATALEN;
    char str[512];
    
    sprintf(str, "%"PRIu32",%"PRIu32"", node_id, srv_data.tail);
    rc = zoo_set(zh, znode_path, str, strlen(str), -1);
    if (rc)
    {
        fprintf(stderr, "Error %d for zoo_set\n", rc);
    }
    struct String_vector *children_list = (struct String_vector *)malloc(sizeof(struct String_vector));
    rc = zoo_get_children(zh, "/election", 0, children_list);
    if (rc)
    {
        fprintf(stderr, "Error %d for zoo_get_children\n", rc);
    }
    char *p;
    struct znodes_data *znodes = (struct znodes_data*)malloc(sizeof(struct znodes_data) * MAX_SERVER_COUNT);

    for (i = 0; i < children_list->count; ++i)
    {
        char *zoo_data = malloc(ZDATALEN * sizeof(char));
        char zpath[512];
        get_znode_path(children_list->data[i], zpath);
        rc = zoo_get(zh, zpath, 0, zoo_data, &zoo_data_len, NULL);
        if (rc)
        {
            fprintf(stderr, "Error %d for zoo_get\n", rc);
        }
        p = strtok(zoo_data, ",");
        znodes[i].node_id = atoi(p);
        p = strtok(NULL, ",");
        znodes[i].tail = atoi(p);
        free(zoo_data);
    }
    
    uint32_t max_tail = znodes[0].tail;
    for (i = 1; i < children_list->count; ++i)
    {
        if (max_tail < znodes[i].tail)
        {
            max_tail = znodes[i].tail;
        }
    }

    for (i = 0; i < children_list->count; ++i)
    {
        if (znodes[i].tail == max_tail)
        {
            cur_view->leader_id = znodes[i].node_id;
        }
    }

    if (cur_view->leader_id == node_id)
    {
        fprintf(stderr, "I am the leader\n");
        //recheck
    }else{
        fprintf(stderr, "I am a follower\n");
        // RDMA read
        // update view
        // zoo_set
    }
    free(znodes);
    return 0;
}

void zoo_wget_children_watcher(zhandle_t *wzh, int type, int state, const char *path, void *watcherCtx) {
    if (type == ZOO_CHILD_EVENT)
    {
        int rc;
        struct watcherContext *watcherPara = (struct watcherContext*)watcherCtx;
        // block the threads
        pthread_mutex_lock(watcherPara->lock);
        rc = zoo_wget_children(zh, "/election", zoo_wget_children_watcher, watcherCtx, NULL);
        if (rc)
        {
            fprintf(stderr, "Error %d for zoo_wget_children\n", rc);
        }
        check_leader(watcherPara->cur_view, watcherPara->node_id, watcherPara->znode_path);
        pthread_mutex_unlock(watcherPara->lock); 
    }
}

int start_zookeeper(int zoo_port, view* cur_view, node_id_t node_id, int *zfd, pthread_mutex_t *lock)
{
	int rc;
	char zoo_host_port[32];
	sprintf(zoo_host_port, "localhost:%d", zoo_port);
	zh = zookeeper_init(zoo_host_port, zookeeper_init_watcher, 15000, 0, 0, 0);

    while(is_connected != 1);
    int interest, fd;
    struct timeval tv;
    zookeeper_interest(zh, &fd, &interest, &tv);
    *zfd = fd;

    char path_buffer[512];
    rc = zoo_create(zh, "/election/guid-n_", NULL, 0, &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE|ZOO_EPHEMERAL, path_buffer, 512);
    if (rc)
    {
        fprintf(stderr, "Error %d for zoo_create\n", rc);
    }
    char znode_path[512];
    get_znode_path(path_buffer, znode_path);

    check_leader(cur_view, node_id, path_buffer);
    struct watcherContext watcherPara;
    strcpy(watcherPara.znode_path, znode_path);
    watcherPara.node_id = node_id;
    watcherPara.lock = lock;
    watcherPara.cur_view = cur_view;

    rc = zoo_wget_children(zh, "/election", zoo_wget_children_watcher, &watcherPara, NULL);
    if (rc)
    {
        fprintf(stderr, "Error %d for zoo_wget_children\n", rc);
    }
    return 0;
}

int initialize_node(node* my_node,const char* log_path, void* db_ptr,void* arg){

    int flag = 1;

    my_node->cur_view.view_id = 1;
    my_node->cur_view.req_id = 0;
    start_zookeeper(my_node->zoo_port, &my_node->cur_view, my_node->node_id, &my_node->zfd, &my_node->lock);

    int build_log_ret = 0;
    if(log_path==NULL){
        log_path = ".";
    }else{
        if((build_log_ret=mkdir(log_path,S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))!=0){
            if(errno!=EEXIST){
                err_log("CONSENSUS MODULE : Log Directory Creation Failed,No Log Will Be Recorded.\n");
            }else{
                build_log_ret = 0;
            }
        }
    }
    if(!build_log_ret){
            char* sys_log_path = (char*)malloc(sizeof(char)*strlen(log_path)+50);
            memset(sys_log_path,0,sizeof(char)*strlen(log_path)+50);
            if(NULL!=sys_log_path){
                sprintf(sys_log_path,"%s/node-%u-consensus-sys.log",log_path,my_node->node_id);
                my_node->sys_log_file = fopen(sys_log_path,"w");
                free(sys_log_path);
            }
            if(NULL==my_node->sys_log_file && (my_node->sys_log || my_node->stat_log)){
                err_log("CONSENSUS MODULE : System Log File Cannot Be Created.\n");
            }
    }

    if (my_node->cur_view.leader_id==my_node->node_id)
    {
        connect_peers(my_node->peer_pool[my_node->node_id].peer_address, 1, my_node->node_id);
    } else{
        connect_peers(my_node->peer_pool[my_node->cur_view.leader_id].peer_address, 0, my_node->node_id);
    }
    
    my_node->consensus_comp = NULL;

    my_node->consensus_comp = init_consensus_comp(my_node,my_node->my_address,&my_node->lock,
            my_node->node_id,my_node->sys_log_file,my_node->sys_log,
            my_node->stat_log,my_node->db_name,db_ptr,my_node->group_size,
            &my_node->cur_view,&my_node->highest_to_commit,&my_node->highest_committed,
            &my_node->highest_seen,arg);
    if(NULL==my_node->consensus_comp){
        goto initialize_node_exit;
    }
    flag = 0;
initialize_node_exit:

    return flag;
}

node* system_initialize(node_id_t node_id,const char* config_path, const char* log_path, void* db_ptr,void* arg){

    node* my_node = (node*)malloc(sizeof(node));
    memset(my_node,0,sizeof(node));
    if(NULL==my_node){
        goto exit_error;
    }

    my_node->node_id = node_id;
    my_node->db_ptr = db_ptr;

    if(pthread_mutex_init(&my_node->lock,NULL)){
        err_log("CONSENSUS MODULE : Cannot Init The Lock.\n");
        goto exit_error;
    }

    if(consensus_read_config(my_node,config_path)){
        err_log("CONSENSUS MODULE : Configuration File Reading Failed.\n");
        goto exit_error;
    }


    if(initialize_node(my_node,log_path,db_ptr,arg)){
        err_log("CONSENSUS MODULE : Network Layer Initialization Failed.\n");
        goto exit_error;
    }

    return my_node;

exit_error:
    if(NULL!=my_node){
    }

    return NULL;
}