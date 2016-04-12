#!/usr/bin/env python
"""
Author: Jingyu
Date: 6 April, 2016
Description: A guard for handling do_checkpoint and do_restore command.

Work Flow:
1. Starting. After RDMA program has been stared. The following information, including node_id, pid and cfg file path, will be prepared as command parameters of the guard.py.

python guard.py node_id pid rdma.cfg

2. Providing services. The guard will start two types of interfaces(inner and outer). The inner interface is an unix socket, which is used for accepting command from the RDMA program in the same machine. The outer interface will be a HTTP service, which implements the operations, such as checkpoint and restore. The detailed interface will be introduced in the next section.


Interface:

Inner: unix socket
1. checkpoint node_id round_id
A RDMA program (the leader) will send this command to $UNIX_SOCK. 
If the node_id is not itself, the command will route to the destnation, through outer.1.

The guard will launch criu to make a checkpoint, then wake up the RDMA program.
Once the checkpoint dump files are created, they will be packed as a zip file then thie file will be copied to others through outer interface with the help of scp command.

2. restore node_id round_id
A RDMA program (the leader) will send this command to $UNIX_SOCK. The guard will 
If the node_id is not itself, the command will route to the destnation, through outer interface.

The guard will launch criu to restore a checkpoint. Firstly, a version which is nearest and smaller than round_id is selected. Then the guard will kill the RDMA program through kill -9. After that, the guard will launch criu command to restore the program.

Outer: TCP socket, HTTP service
1. checkpoint?node_id=XX&round_id=XXXX
A URL such as http://$BIND_HOST:$BIND_PORT/checkpoint?node_id=XX&round_id=XXXX will be created and be sent to node_id machine. The guard will firstly reply this request. Then start a thread to do checkpoint which is descripted in Inner.1

2. restore?node_id=XX&round_id=XXXX
A URL such as http://$BIND_HOST:$BIND_PORT/restore?node_id=XX&round_id=XXXX will be created and be sent to node_id machine. The guard will firstly reply this request. The start a thread to do restore which is descripted in Inner.2

"""

from wsgiref.simple_server import make_server
from cgi import parse_qs, escape
import SocketServer
from multiprocessing import Process
import subprocess
import time
import sys
import os
import tempfile
import zipfile
import shutil


# They are global variables, and will be initialised in init()
#=================
# The location of RDMA configuration file path, I need parse ip and port, and then find PID by lsof
RDMA_CFG=""
# PID of the process which will be checkpoint
AIM_PID=-1
# self id repesents the guard
SELF_ID=-1 
# Dir for storing checkpoint zip files.
STORE_BASE="/tmp/checkpoint_store"
#=================
# The IP:PORT I am listening for service.
BIND_HOST="0.0.0.0"
BIND_PORT=12345
# The unix socket I am listening for accepting internal request.
UNIX_SOCK="/tmp/guard.sock"

DEF_INDEX="""<!DOCTYPE html>
<html>
<body>
<h1>Guard works.</h1>
<div>
<div>
<a href='/checkpoint?node_id=1&round_id=1'>checkpoint</a>
</div>
<div>
<a href='/restore?node_id=1&round_id=1'>restore</a>
</div>
</div>
</body>
</html>
"""

# The init function will parse $RDMA_CFG
def init():
	global SELF_ID, AIM_PID, RDMA_CFG
	try:
		argv = sys.argv
		SELF_ID=int(argv[1])
		AIM_PID=int(argv[2])
		RDMA_CFG=argv[3]
	except Exception as e:
		print "[init] error: %s"%(str(e))
		exit()
	print "The guard has got self_id:%d, aim_pid:%d, and cfg file: %s"%(SELF_ID,AIM_PID,RDMA_CFG)
	if os.path.exists(STORE_BASE):
		shutil.rmtree(STORE_BASE)
	else: # dir is not existed, we need create one
		os.mkdir(STORE_BASE)

# default handler of index.html
def index(environ, start_response):
	start_response('200 OK',[('Content-Type', 'text/html')])
	return [DEF_INDEX] 

# default handler of 404
def not_found(environ, start_response):
	start_response('404 NOT FOUND', [('Content-Type', 'text/plain')])
	return ['Not Found']

def outer_parse(environ):
	parameters = parse_qs(environ.get('QUERY_STRING', ''))
	try:
		node_id = escape(parameters['node_id'][0])
		round_id = escape(parameters['round_id'][0])
		node_id = int(node_id)
		round_id = int(round_id)
		return (node_id,round_id)
	except Exception as e:
		print "[outer_checkpoint] error: %s"%(str(e))
		return (-1,-1)

def outer_checkpoint(environ, start_response):
	start_response('200 OK',[('Content-Type', 'text/html')])
	node_id, round_id = outer_parse(environ)
	print "[outer_checkpoint] node_id:%d, round_id:%d"%(node_id,round_id)
	if SELF_ID == node_id:
		inner_checkpoint(node_id,round_id)
	else:
		print "[outer_checkpoint] access deny, id is invalid SELF_ID:%d, node_id:%d"%(SELF_ID,node_id)
	return ['checkpoint ok']	

def outer_restore(environ, start_response):
	start_response('200 OK',[('Content-Type', 'text/html')])
	node_id, round_id = outer_parse(environ)
	print "[outer_restore] node_id:%d, round_id:%d"%(node_id,round_id)
	if SELF_ID == node_id:
		inner_restore(node_id,round_id)
	else:
		print "[outer_checkpoint] access deny, id is invalid SELF_ID:%d, node_id:%d"%(SELF_ID,node_id)
	return ['restore ok']

# The implementation is based on http://lucumr.pocoo.org/2007/5/21/getting-started-with-wsgi/
def application(environ, start_response):
	"""
	The main WSGI application. This function will dispatch the current request to currect handler.
	"""
	urls=[
		('index', index),
		('checkpoint', outer_checkpoint),
		('restore',outer_restore)
	]
	path = environ.get("PATH_INFO")
	for prefix, callback in urls:
		match = path[1:].startswith(prefix)
		if match:
			return callback(environ, start_response)
	return not_found(environ, start_response)

def getNextBaseName():
	max_id = 0
	for root, dirs, files in os.walk(STORE_BASE):
		for name in files:
			print "[getNextBaseName] name: %s"%(name)
			if name.startswith("checkpoint_"):
				prefix_size = len("checkpoint_")
				sufix_size = len(".zip")
				file_id = name[prefix_size+1:len(name)-sufix_size]
				absName = os.path.join(root,name)
				print "[getNextBaseName] absName: %s, current file_id: %s"%(absName,file_id)	
				try:
					file_id = int(file_id)
					if max_id < file_id:
						max_id = file_id
				except Exception as e:
					pass
	next_id = max_id+1
	return os.path.join(STORE_BASE,"checkpoint_%d"%(next_id))

def inner_checkpoint(node_id,round_id):
	print "[inner_checkpoint] criu will be used for checkpointing pid: %d at machine %d, at round %d"%(AIM_PID,node_id,round_id)
	# mkdir dump
	tmpDir = None
	try:
		tmpDir = tempfile.mkdtemp()
	except Exception as e:
		pass
	if tmpDir:
		cmd="/sbin/criu dump -v4 --leave-running -D %s -t %d"%(tmpDir,AIM_PID)
		print "[inner_checkpoint]cmd: %s"%(cmd)
		subprocess.call(cmd,shell=True)
		zipBaseName = getNextBaseName() # without extName
		zipAbsName = shutil.make_archive(zipBaseName,'zip',tmpDir)
		print "[inner_checkpoint] checkpoint %s is created."%(zipAbsName)
		shutil.rmtree(tmpDir)
	else:
		print "[inner_checkpoint]creat tmpDir failed."
	return

def inner_restore(node_id,round_id):
	print "[inner_restore] criu will be used for restoring  at machine %d, at round %d"%(node_id,round_id)
	cmd="/sbin/criu restore -v4 -d -D ./dump"
	print "[inner_restore]cmd: %s"%(cmd)
	subprocess.call(cmd,shell=True)
	return 

def inner_service(cmd,node_id,round_id):
	urls=[
		('checkpoint',inner_checkpoint),
		('restore',inner_restore)	
	]
	path = cmd
	for prefix, callback in urls:
		match = (path == prefix)
		if match:
			callback(node_id,round_id)
	return

def getIPbyID(node_id):
	return "10.22.1.%d"%(node_id+1)	
def getPortbyID(node_id):
	return 12345

def route(cmd,node_id,round_id):
	ip_str=getIPbyID(node_id)	
	port_int=getPortbyID(node_id)
	url = "http://%s:%d/%s?node_id=%d&round_id=%d"%(ip_str,port_int,cmd,node_id,round_id)
	print "[route] url:%s"%(url)
	return

# The handler for unix socket
class InnerHandler(SocketServer.BaseRequestHandler):
	def handle(self):
		print "[inner] Client is connected."
		recv_size=4096
		request = self.request.recv(recv_size).strip()
		print "[inner] recv:#%s#"%(request)
		parts = request.split()
		if 3!=len(parts):
			print "[inner] Invalied parameters."
			self.request.sendall("[inner] ERROR")
			return
		else:
			(cmd,node_id,round_id) = parts
			print "[inner] cmd: %s , node_id: %s, round_id: %s"%(cmd,node_id,round_id)
			try:
				node_id = int(node_id)
				round_id = int(round_id)
			except Exception as e:
				print "[init] error: %s"%(str(e))
				self.request.sendall("[inner] ERROR")
				return
			print "[inner] self_id: %d, node_id: %d"%(SELF_ID,node_id)
			if SELF_ID == node_id: # This is a inner call
				inner_service(cmd,node_id,round_id)	
				self.request.sendall("[inner] OK. The calling is served.")
			else: # This is a outer call
				route(cmd,node_id,round_id)
				self.request.sendall("[inner] OK. The calling is routed.")

# The function will start outer interface handler in a thread
def start_outer(args):
	print "[outer] Interface(http://%s:%d) will start."%(BIND_HOST,BIND_PORT)
	srv = make_server(BIND_HOST, BIND_PORT, application)
	srv.serve_forever()

# The function will start inner interface handler in a thread
def start_inner(args):
	try:
		os.unlink(UNIX_SOCK)
	except OSError:
		if os.path.exists(UNIX_SOCK):
			print "[inner] ERR file %s cannot be removed."%(UNIX_SOCK)
			print "[inner] Interface failed."
			return
	print "[inner] Interface(%s) will start."%(UNIX_SOCK)
	srv = SocketServer.UnixStreamServer(UNIX_SOCK,InnerHandler)	
	srv.serve_forever()

# print the usage of this python program
def usage():
	print "Usage:\npython guard.py node_id pid rdma.cfg"

if __name__ == '__main__':
	argc = len(sys.argv)
	if 4!=argc:
		usage()
		exit()
	else:
		init()
		outer = Process(target=start_outer,args=((),))
		outer.start()
		inner = Process(target=start_inner,args=((),))
		inner.start()
		outer.join()
		inner.join()
