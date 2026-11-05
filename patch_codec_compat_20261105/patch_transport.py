"""Rare patch transactions; owners poll between frames, never wait for network IO."""
import collections,concurrent.futures,json,os,re,socket,threading,time,uuid
from pathlib import Path
BASE=Path('/dev/shm/autoeyez_patch')
POOL=concurrent.futures.ThreadPoolExecutor(max_workers=2)

def atomic_json(path,value,durable=False):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    try:
        with open(temp,'w') as f:
            json.dump(value,f,allow_nan=False,separators=(',',':'));f.flush()
            if durable:os.fsync(f.fileno())
        os.replace(temp,path)
        if durable:
            fd=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
            try:os.fsync(fd)
            finally:os.close(fd)
    finally:
        try:temp.unlink()
        except FileNotFoundError:pass

def validate_request(req):
    if not isinstance(req,dict) or type(req.get('version')) is not int or req.get('version')!=1:raise ValueError('RPC version')
    if not isinstance(req.get('tx'),str) or not re.fullmatch('[a-f0-9]{16}',req['tx']):raise ValueError('transaction identity')
    if req.get('phase') not in ('prepare','commit','abort','rollback','finalize'):raise ValueError('transaction phase')
    return req

class FrameInbox:
    def __init__(self,name,prepare,apply,dispose=lambda value:None):
        self.root=BASE/name;self.root.mkdir(parents=True,exist_ok=True)
        self.prepare,self.apply,self.dispose=prepare,apply,dispose
        self.stages={};self.replies=collections.OrderedDict();self.waiting=None
    def poll(self):
        request=self.root/'request.json'
        if self.waiting:
            req,future=self.waiting
            if not future.done():return
            self.waiting=None
            try:
                candidate,before=future.result()
                if req['phase']=='prepare':self._prepared(req,candidate,before)
                else:
                    if time.monotonic()>req['deadline']:self.dispose(candidate);raise TimeoutError('rollback preparation expired')
                    self.apply(candidate);self.dispose(candidate);self._drop(req['tx']);self._reply(req,{'ok':True})
            except Exception as e:self._reply(req,{'ok':False,'error':str(e)})
            return
        try:
            processing=self.root/'processing.json'
            os.replace(request,processing)
            with open(processing) as f:req=json.load(f)
            processing.unlink()
        except FileNotFoundError:return
        except (OSError,ValueError):return
        try:
            validate_request(req);key=(req['tx'],req['phase'])
            if key in self.replies:self._reply(req,self.replies[key]);return
            if time.monotonic()>req['deadline']:raise TimeoutError('expired owner request')
            tx,phase=req['tx'],req['phase']
            if phase=='prepare':
                if self.stages and tx not in self.stages:raise RuntimeError('another patch pending')
                result=self.prepare(req['payload'])
                if isinstance(result,concurrent.futures.Future):self.waiting=(req,result);return
                self._prepared(req,*result);return
            if phase=='rollback':
                result=self.apply(req['payload'])
                if isinstance(result,concurrent.futures.Future):self.waiting=(req,result);return
                self._drop(tx)
            elif phase=='commit':
                stage=self.stages.get(tx)
                if stage is None:raise RuntimeError('patch was not prepared')
                self.apply(stage[0])
            elif phase in ('abort','finalize'):self._drop(tx)
            self._reply(req,{'ok':True})
        except Exception as e:self._reply(req,{'ok':False,'error':str(e)})
    def _drop(self,tx):
        stage=self.stages.pop(tx,None)
        if stage:self.dispose(stage[0])
    def _prepared(self,req,candidate,before):
        if time.monotonic()>req['deadline']:
            self.dispose(candidate);raise TimeoutError('preparation expired')
        self.stages[req['tx']]=(candidate,before)
        self._reply(req,{'ok':True,'before':before})
    def _reply(self,req,result):
        if not isinstance(req,dict) or not isinstance(req.get('tx'),str) or not isinstance(req.get('phase'),str):return
        if result.get('ok'):self.replies[(req['tx'],req['phase'])]=result
        while len(self.replies)>64:self.replies.popitem(last=False)
        try:atomic_json(self.root/'reply.json',dict(result,tx=req['tx'],phase=req['phase'],rpc=req.get('rpc','')))
        except (OSError,ValueError) as e:print('PATCH reply unavailable: '+str(e),flush=True)

LOCKS={name:threading.Lock() for name in ('waaave','mixer','player','cues')}
def owner_request(name,req,timeout=4):
    validate_request(req)
    if name not in LOCKS:raise ValueError('owner')
    with LOCKS[name]:
        root=BASE/name;root.mkdir(parents=True,exist_ok=True)
        req=dict(req,deadline=time.monotonic()+timeout,rpc=uuid.uuid4().hex)
        atomic_json(root/'request.json',req)
        while time.monotonic()<req['deadline']:
            try:
                with open(root/'reply.json') as f:result=json.load(f)
                if result.get('tx')==req['tx'] and result.get('phase')==req['phase'] and result.get('rpc')==req['rpc']:
                    if not result['ok']:raise RuntimeError(result.get('error','owner failed'))
                    return result
            except (FileNotFoundError,ValueError):pass
            time.sleep(.005)
        raise TimeoutError('owner '+name+' did not acknowledge; result uncertain')

def remote_request(host,name,req):
    with socket.create_connection((host,5012),timeout=5) as sock:
        sock.settimeout(5);sock.sendall(json.dumps({'owner':name,'request':req}).encode()+b'\n')
        data=bytearray()
        while not data.endswith(b'\n'):
            part=sock.recv(4096)
            if not part or len(data)+len(part)>32768:raise ConnectionError('invalid remote response')
            data.extend(part)
        result=json.loads(data)
        if not result.get('ok'):raise RuntimeError(result.get('error','remote failed'))
        return result['result']

def serve_remote():
    listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    listener.bind(('10.0.0.2',5012));listener.listen(4)
    capacity=threading.BoundedSemaphore(4)
    def handle(conn,peer):
        with conn:
            try:
                if peer[0]!='10.0.0.1':raise ValueError('unexpected patch sender')
                conn.settimeout(5);data=bytearray()
                while not data.endswith(b'\n'):
                    part=conn.recv(4096)
                    if not part or len(data)+len(part)>16384:raise ValueError('invalid patch packet')
                    data.extend(part)
                packet=json.loads(data);name=packet['owner']
                if name not in ('mixer','player','cues'):raise ValueError('remote owner')
                result={'ok':True,'result':owner_request(name,packet['request'])}
            except Exception as e:result={'ok':False,'error':str(e)}
            try:conn.sendall(json.dumps(result).encode()+b'\n')
            except OSError:pass
            finally:capacity.release()
    while True:
        conn,peer=listener.accept()
        if capacity.acquire(blocking=False):threading.Thread(target=handle,args=(conn,peer),daemon=True).start()
        else:
            with conn:
                try:conn.sendall(b'{"ok":false,"error":"patch RPC busy"}\n')
                except OSError:pass
