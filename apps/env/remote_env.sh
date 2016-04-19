#!/bin/bash
if [ -f ~/.bashrc ]; then
  source ~/.bashrc
fi
#information in '' will direct send to remote while "" will parse in local first
ssh $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host1) "mkdir -p $RDMA_ROOT"
scp -r $RDMA_ROOT $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host1):$RDMA_ROOT/..
ssh $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host1) "cd $RDMA_ROOT/apps/env && ./local_env.cfg"
ssh $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host1) 'source ~/.bashrc'

ssh $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host2) "mkdir -p $RDMA_ROOT"
scp -r $RDMA_ROOT $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host2):$RDMA_ROOT/..
ssh $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host2) "cd $RDMA_ROOT/apps/env && ./local_env.cfg"
ssh $LOGNAME@$(cat $RDMA_ROOT/apps/env/remote_host2) 'source ~/.bashrc'
