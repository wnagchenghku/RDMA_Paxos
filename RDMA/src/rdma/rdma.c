#include "../include/rdma/rdma_common.h"

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

static struct resources res;

struct configuration_t config =
{
	NULL, /* dev_name (default first device found) */
	1, /* ib_port (default 1) */
	-1, /* gid_idx (default not used) */
	-1 /* node id*/
};

static void resources_init(struct resources *res)
{
	memset(res, 0, sizeof *res);
	res->sock = -1;
}

static int resources_create(struct resources *res)
{
	struct ibv_device **dev_list = NULL;
	struct ibv_device *ib_dev = NULL;

	size_t size;
	int i;
	int mr_flags = 0;
	int num_devices;
	int rc = 0;

	dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list)
	{
		fprintf(stderr, "failed to get IB devices list\n");
		rc = 1;
		goto resources_create_exit;
	}

	if (!num_devices)
	{
		fprintf(stderr, "found %d devices\n", num_devices);
		rc = 1;
		goto resources_create_exit;
	}

	fprintf(stdout, "found %d device(s)\n", num_devices);

	for (i = 0; i < num_devices; i ++)
	{
		if(!config.dev_name) {
			config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
			fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name);
		}

		if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name))
		{
			ib_dev = dev_list[i];
			break;
		}
	}

	if (!ib_dev)
	{
		fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}
	/* get device handle */
	res->ib_ctx = ibv_open_device(ib_dev);
	if (!res->ib_ctx)
	{
		fprintf(stderr, "failed to open device %s\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}

	/* We are now done with device list, free it */
	ibv_free_device_list(dev_list);
	dev_list = NULL;
	ib_dev = NULL;

	/* query port properties */
	if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr)) {
		fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
		rc = 1;
		goto resources_create_exit;
	}

	/* allocate Protection Domain */
	res->pd = ibv_alloc_pd(res->ib_ctx);
	if (!res->pd)
	{
		fprintf(stderr, "ibv_alloc_pd failed\n");
		rc = 1;
		goto resources_create_exit;
	}

	size = LOG_SIZE;
	res->buf = (char*)malloc(size);
	if (!res->buf)
	{
		fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
		rc = 1;
		goto resources_create_exit;
	}

	memset(res->buf, 0, size);

	mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
	if (!res->mr)
	{
		fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
		rc = 1;
		goto resources_create_exit;
	}
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n", res->buf, res->mr->lkey, res->mr->rkey, mr_flags);	


    if (0 != find_max_inline(res->ib_ctx, res->pd, &res->rc_max_inline_data))
    {
        fprintf(stderr, "Cannot find max RC inline data\n");
    }
    fprintf(stdout, "# MAX_INLINE_DATA = %"PRIu32"\n", res->rc_max_inline_data);

resources_create_exit:
	if (rc)
	{
		/* Error encountered, cleanup */
		if(res->mr)
		{
			ibv_dereg_mr(res->mr);
			res->mr = NULL;
		}

		if(res->buf)
		{
			free(res->buf);
			res->buf = NULL;
		}

		if (res->pd)
		{
			ibv_dealloc_pd(res->pd);
			res->pd = NULL;
		}

		if (res->ib_ctx)
		{
			ibv_close_device(res->ib_ctx);
			res->ib_ctx = NULL;
		}

		if (dev_list)
		{
			ibv_free_device_list(dev_list);
			dev_list = NULL;
		}
	}
	return rc;
}


static int modify_qp_to_init(struct ibv_qp *qp)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_INIT;
	attr.port_num = config.ib_port;
	attr.pkey_index = 0;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "Failed to modify QP state to INIT\n");

	return rc;
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_256; /* Maximum Transmission Unit (MTU) supported by port. Can be: IBV_MTU_256, IBV_MTU_512, IBV_MTU_1024 */
	attr.dest_qp_num = remote_qpn;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1; /* Number of responder resources for handling incoming RDMA reads & atomic operations (valid only for RC QPs) */
	attr.min_rnr_timer = 0x12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = dlid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = config.ib_port;
	if (config.gid_idx >= 0)
	{
		attr.ah_attr.is_global = 1;
		attr.ah_attr.port_num = 1;
		memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
		attr.ah_attr.grh.flow_label = 0;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.sgid_index = config.gid_idx;
		attr.ah_attr.grh.traffic_class = 0;
	}
	
	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTR\n");

	return rc;
}

static int modify_qp_to_rts(struct ibv_qp *qp)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 1; // ~ 8 us
	attr.retry_cnt = 0; // max is 7
	attr.rnr_retry = 7;
	attr.sq_psn = 0;
	attr.max_rd_atomic = 1;

	flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTS\n");
	return rc;
}

int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data) {
	int rc;
	int read_bytes = 0;
	int total_read_bytes = 0;
	rc = write(sock, local_data, xfer_size);
	if(rc < xfer_size)
		fprintf(stderr, "Failed writing data during sock_sync_data\n");
	else
		rc = 0;
	while(!rc && total_read_bytes < xfer_size) {
		read_bytes = read(sock, remote_data, xfer_size);
		if(read_bytes > 0)
			total_read_bytes += read_bytes;
		else
			rc = read_bytes;
	}
	return rc;
}

static int connect_qp(struct resources *res)
{
	struct cm_con_data_t local_con_data;
	struct cm_con_data_t remote_con_data;
	struct cm_con_data_t tmp_con_data;
	int rc = 0, cq_size = 0;
	union ibv_gid my_gid;

	if (config.gid_idx >= 0)
	{
		rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
		if (rc)
		{
			fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
			return rc;
		}
	}
	else
		memset(&my_gid, 0, sizeof my_gid);

	local_con_data.node_id = htonl(config.node_id);
	local_con_data.addr = htonll((uintptr_t)res->buf);
	local_con_data.rkey = htonl(res->mr->rkey);

	/* Completion Queue with CQ_CAPACITY entry */
	cq_size = CQ_CAPACITY;
	struct ibv_cq * tmp_cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
	if (!tmp_cq)
	{
		fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
		rc = 1;
		goto connect_qp_exit;
	}

	/* create the Queue Pair */
	struct ibv_qp_init_attr qp_init_attr;
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));

	qp_init_attr.qp_type = IBV_QPT_RC;
	//qp_init_attr.sq_sig_all = 0;
	qp_init_attr.send_cq = tmp_cq;
	qp_init_attr.recv_cq = tmp_cq;
	qp_init_attr.cap.max_inline_data = res->rc_max_inline_data;
	qp_init_attr.cap.max_send_wr = Q_DEPTH; /* Maximum send posting capacity */
	qp_init_attr.cap.max_recv_wr = 1;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	struct ibv_qp *tmp_qp = ibv_create_qp(res->pd, &qp_init_attr);
	if (!tmp_qp)
	{
		fprintf(stderr, "failed to create QP\n");
		rc = 1;
		goto connect_qp_exit;
	}

	local_con_data.qp_num = htonl(tmp_qp->qp_num);
	local_con_data.lid = htons(res->port_attr.lid);
	memcpy(local_con_data.gid, &my_gid, 16);
	fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);

	if (sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char*)&local_con_data, (char*)&tmp_con_data))
	{
		fprintf(stderr, "failed to exchange connection data between sides\n");
		rc = 1;
		goto connect_qp_exit;
	}

	remote_con_data.addr = ntohll(tmp_con_data.addr);
	remote_con_data.rkey = ntohl(tmp_con_data.rkey);
	remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
	remote_con_data.lid = ntohs(tmp_con_data.lid);
	remote_con_data.node_id = ntohl(tmp_con_data.node_id);
	memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

	res->remote_props[remote_con_data.node_id] = remote_con_data; /* values to connect to remote side */

	fprintf(stderr, "Node id = %"PRIu32"\n", remote_con_data.node_id);
	fprintf(stdout, "Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
	fprintf(stdout, "Remote rkey = 0x%x\n", remote_con_data.rkey);
	fprintf(stdout, "Remote QP number = 0x%x\n", remote_con_data.qp_num);
	fprintf(stdout, "Remote LID = 0x%x\n", remote_con_data.lid);
	if (config.gid_idx >= 0)
	{
		uint8_t *p = remote_con_data.gid;
		fprintf(stdout, "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}

	res->qp[remote_con_data.node_id] = tmp_qp;
	res->cq[remote_con_data.node_id] = tmp_cq;
	fprintf(stdout, "Local QP for %"PRIu32" was created, QP number=0x%x\n", remote_con_data.node_id, res->qp[remote_con_data.node_id]->qp_num);

	rc = modify_qp_to_init(res->qp[remote_con_data.node_id]);
	if (rc)
	{
		fprintf(stderr, "change QP state to INIT failed\n");
		goto connect_qp_exit;
	}
	rc = modify_qp_to_rtr(res->qp[remote_con_data.node_id], remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
	if (rc)
	{
		fprintf(stderr, "failed to modify QP state to RTR\n");
		goto connect_qp_exit;
	}

	rc = modify_qp_to_rts(res->qp[remote_con_data.node_id]);
	if (rc)
	{
		fprintf(stderr, "failed to modify QP state to RTS\n");
		goto connect_qp_exit;
	}
	fprintf(stdout, "QP state was change to RTS\n");

connect_qp_exit:
	return rc;
}

static int resources_destroy(struct resources *res)
{
	int i, rc = 0;

	for (i = 0; i < MAX_SERVER_COUNT; ++i)
	{
		if (res->qp[i])
		{
			if (ibv_destroy_qp(res->qp[i]))
			{
				fprintf(stderr, "failed to destroy QP\n");
				rc = 1;
			}
		}
	}

	if (res->mr)
		if (ibv_dereg_mr(res->mr)) {
			fprintf(stderr, "failed to deregister MR\n");
			rc = 1;
		}

	if (res->buf)
		free(res->buf);

	for (i = 0; i < MAX_SERVER_COUNT; ++i)
	{
		if (res->cq[i])
			if (ibv_destroy_cq(res->cq[i]))
			{
				fprintf(stderr, "failed to destroy CQ\n");
				rc = 1;
			}
	}

	if (res->pd)
		if (ibv_dealloc_pd(res->pd))
		{
			fprintf(stderr, "failed to deallocate PD\n");
			rc = 1;
		}

	if (res->ib_ctx)
		if (ibv_close_device(res->ib_ctx))
		{
			fprintf(stderr, "failed to close device context\n");
			rc = 1;
		}

	return rc;
}

void *connect_peers(peer* peer_pool, uint32_t node_id, uint32_t group_size)
{
	config.node_id = node_id;
	config.gid_idx = 0;
	
	resources_init(&res);

	if (resources_create(&res))
	{
		fprintf(stderr, "failed to create resources\n");
		goto connect_peers_exit;
	}

	int i, sockfd = -1, count = node_id;
	for (i = 0; count > 0; i++)
	{
		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0)
		{
			fprintf(stderr, "ERROR opening socket\n");
			goto connect_peers_exit;
		}
		if (i == config.node_id)
			continue;
		while (connect(sockfd, (struct sockaddr *)peer_pool[i].peer_address, sizeof(struct sockaddr_in)) < 0);
		res.sock = sockfd;
		if (connect_qp(&res))
		{
			fprintf(stderr, "failed to connect QPs\n");
			goto connect_peers_exit;
		}
		if (close(sockfd))
		{
			fprintf(stderr, "failed to close socket\n");
			goto connect_peers_exit;
		}
		count--;
	}

	struct sockaddr_in clientaddr;
	socklen_t clientlen = sizeof(clientaddr);
	int newsockfd = -1;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (bind(sockfd, (struct sockaddr *)peer_pool[node_id].peer_address, sizeof(struct sockaddr_in)) < 0)
	{
		perror ("ERROR on binding");
		goto connect_peers_exit;
	}
	listen(sockfd, 5);

	count = group_size - node_id;
	while (count > 1)
	{
		newsockfd = accept(sockfd, (struct sockaddr *)&clientaddr, &clientlen);
		res.sock = newsockfd;
		if (connect_qp(&res))
		{
			fprintf(stderr, "failed to connect QPs\n");
			goto connect_peers_exit;
		}
		if (close(newsockfd))
		{
			fprintf(stderr, "failed to close socket\n");
			goto connect_peers_exit;
		}
		count--;	
	}
	if (close(sockfd))
	{
		fprintf(stderr, "failed to close socket\n");
		goto connect_peers_exit;
	}

	return &res;

connect_peers_exit:
	if (resources_destroy(&res)) {
		fprintf(stderr, "failed to destroy resources\n");
	}

	if(config.dev_name)
		free((char*)config.dev_name);

	return NULL;
}