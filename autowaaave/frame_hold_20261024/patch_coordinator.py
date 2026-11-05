"""Serialized, journaled patch recalls and truthful controller completion replies."""
import hashlib,json,queue,re,threading,time,concurrent.futures
from pathlib import Path
from patch_schema import video_patch, LEGACY_KEYS, V3_KEYS
from patch_transport import atomic_json,owner_request,remote_request
class PatchCoordinator:
    def __init__(self,send,remote='10.0.0.2',journal='/home/admin/.config/autoeyez/patch_transactions',profiles=None):
        self.send,self.remote,self.root,self.profiles=send,remote,Path(journal),profiles
        try:self.root.mkdir(parents=True,exist_ok=True)
        except OSError as e:print('PATCH journal unavailable: '+str(e),flush=True)
        self.queue=queue.Queue(8);self.active=set();self.lock=threading.Lock()
        for path in self.root.glob('*.json'):
            try:
                record=json.loads(path.read_text());tx=path.stem
                if record.get('status') in ('PREPARING','COMMITTING','UNCERTAIN') and re.fullmatch('[a-f0-9]{16}',tx):
                    self.active.add(tx);self.queue.put_nowait((tx,'RECOVER',record['slot'],None,record['identity']))
            except (OSError,ValueError,KeyError,queue.Full):pass
        self.thread=threading.Thread(target=self._run,daemon=True,name='patch-coordinator');self.thread.start()
    def receive(self,line):
        try:
            parts=line.split(':',4)
            if len(parts)<4 or parts[0]!='PTX' or not re.fullmatch('[a-f0-9]{16}',parts[1]) or parts[2] not in ('VLOAD','VSAVE','ALOAD','ASAVE'):raise ValueError('patch wire envelope')
            tx,op=parts[1:3];slot=int(parts[3])
            if str(slot)!=parts[3] or not 0<=slot<32:raise ValueError('patch wire slot')
            patch=None
            if op.startswith('V'):
                if len(parts)!=5:raise ValueError('missing video snapshot')
                raw=parts[4].split(',')
                if any(not re.fullmatch('-?(?:0|[1-9][0-9]*)',v) for v in raw):raise ValueError('patch wire integer')
                patch=video_patch([int(v) for v in raw],slot)
            elif len(parts)!=4:raise ValueError('extra audio fields')
            identity=hashlib.sha256(line.encode()).hexdigest();path=self.root/(tx+'.json')
            with self.lock:
                if path.exists():
                    state=json.loads(path.read_text())
                    if state['identity']!=identity:raise ValueError('transaction ID reused with different snapshot')
                    if state['status'] in ('OK','ERROR'):self._ack(tx,state['status']);return
                if tx in self.active:return
                self.active.add(tx)
                try:self.queue.put_nowait((tx,op,slot,patch,identity))
                except queue.Full:self.active.remove(tx);raise RuntimeError('patch queue full')
        except Exception as e:
            print('PATCH rejected: '+str(e),flush=True)
            if len(line.split(':'))>1 and line.split(':')[1] not in self.active and re.fullmatch('[a-f0-9]{16}',line.split(':')[1]):self._ack(line.split(':')[1],'ERROR')
    def recover(self,tx,undo=False):
        if not re.fullmatch('[a-f0-9]{16}',tx):return
        try:
            path=self.root/(tx+'.json')
            if not path.exists():self._ack(tx,'ERROR');return
            record=json.loads(path.read_text())
            if not undo and record['status'] in ('OK','ERROR'):self._ack(tx,record['status']);return
            with self.lock:
                if tx in self.active:return
                self.active.add(tx)
                try:self.queue.put_nowait((tx,'UNDO' if undo else 'RECOVER',record['slot'],None,record['identity']))
                except queue.Full:self.active.remove(tx);raise
        except Exception as e:print('PATCH recovery pending: '+str(e),flush=True)
    def _ack(self,tx,status):
        try:self.send('PTX_ACK:'+tx+':'+status)
        except Exception as e:print('PATCH acknowledgment pending: '+str(e),flush=True)
    def _call(self,target,tx,phase,payload=None):
        req={'version':1,'tx':tx,'phase':phase}
        if payload is not None:req['payload']=payload
        return owner_request('waaave',req) if target=='waaave' else remote_request(self.remote,target,req)
    def _write(self,tx,state):atomic_json(self.root/(tx+'.json'),state,durable=True)
    def _run(self):
        while True:
            tx,op,slot,patch,identity=self.queue.get();state={'identity':identity,'status':'PREPARING','operation':op,'slot':slot,'before':{}}
            prepared=[];committed=[];saved_path=None;saved_before=None
            try:
                # Never replay a stale in-progress transaction after bridge restart.
                oldpath=self.root/(tx+'.json')
                if oldpath.exists():
                    old=json.loads(oldpath.read_text());state=old
                    for target in reversed(old.get('attempted',list(old.get('before',{})))):
                        before=old.get('before',{}).get(target)
                        reverting=before is not None and old['status']!='PREPARING' and old['operation']!='ASAVE'
                        self._call(target,tx,'rollback' if reverting else 'abort',before if reverting else None)
                    if 'audio_previous' in old:
                        path=Path('/home/pi/audiopatches')/('AUDIO%02d.TXT'%(slot+1))
                        if old['audio_previous'] is None:path.unlink(missing_ok=True)
                        else:self._save_audio(path,old['audio_previous'])
                    state['status']='ERROR';self._write(tx,state);self._ack(tx,'ERROR');continue
                self._write(tx,state)
                targets=[]
                # Version 2 cue identities were calculated before the five new controls existed.
                # Keep their saved identity stable when recalling a legacy video patch.
                identity_patch=({key:patch[key] for key in (LEGACY_KEYS if patch['patchVersion']<3 else V3_KEYS)} if patch is not None and patch['patchVersion']<4 else patch)
                patch_identity=hashlib.sha256(json.dumps(identity_patch,sort_keys=True,separators=(',',':')).encode()).hexdigest() if patch is not None else None
                if op=='VLOAD':
                    targets=[('waaave',{'kind':'video','patch':patch}),('mixer',{'ch_a':patch['chA'],'ch_b':patch['chB'],'mix':patch['mix'],'luma':patch['lumaKey'] if patch['lumaLowEn'] else 0,'luma_high':patch['lumaHigh'],'luma_src':patch['lumaSrc'],'luma_high_enabled':bool(patch['lumaHiEn'])}),('player',{'index':patch['clip'],'loop':bool(patch['loop'])}),('cues',{'slot':slot,'operation':'load','patch_identity':patch_identity,'require_identity':patch['patchVersion']>=2})]
                elif op=='VSAVE':targets=[('cues',{'slot':slot,'operation':'save','patch_identity':patch_identity,'require_identity':True})]
                else:targets=[('waaave',{'kind':'audio_save' if op=='ASAVE' else 'audio_load','slot':slot+1})]
                for target,payload in targets:
                    prepared.append(target);state.setdefault('attempted',[]).append(target);self._write(tx,state);result=self._call(target,tx,'prepare',payload);state['before'][target]=result['before'];self._write(tx,state)
                state['status']='COMMITTING';self._write(tx,state)
                # Dispatch independent owners together, limiting cross-device
                # phase offset to their frame boundaries rather than a sweep.
                committed=[target for target,_ in targets]
                errors=[]
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    jobs=[pool.submit(self._call,target,tx,'commit') for target in committed]
                    for job in jobs:
                        try:job.result()
                        except Exception as error:errors.append(str(error))
                if errors:raise RuntimeError('; '.join(errors))
                if op=='ASAVE':
                    target_path=Path('/home/pi/audiopatches')/('AUDIO%02d.TXT'%(slot+1))
                    if target_path.exists() and target_path.stat().st_size>16384:raise ValueError('existing audio patch too large; retained')
                    previous_text=target_path.read_text() if target_path.exists() else None
                    state['audio_previous']=previous_text;self._write(tx,state)
                    saved_path=target_path;saved_before=previous_text
                    text=state['before']['waaave']['candidate_text']
                    self._save_audio(saved_path,text)
                if op=='ALOAD' and self.profiles:self.profiles(state['before']['waaave']['candidate_text'])
                state['status']='OK';self._write(tx,state)
                for target in prepared:
                    try:self._call(target,tx,'finalize')
                    except Exception as e:print('PATCH finalize retry needed '+str(e),flush=True)
                self._ack(tx,'OK')
                print('PATCH '+op+' slot='+str(slot)+' committed '+tx,flush=True)
            except Exception as e:
                errors=[]
                if saved_path is not None:
                    try:
                        if saved_before is None:saved_path.unlink(missing_ok=True)
                        elif not saved_path.exists() or saved_path.read_text()!=saved_before:self._save_audio(saved_path,saved_before)
                    except Exception as rollback:errors.append(str(rollback))
                for target in reversed(prepared):
                    try:self._call(target,tx,'rollback' if target in committed and op!='ASAVE' else 'abort',state['before'][target] if target in committed and op!='ASAVE' else None)
                    except Exception as rollback:errors.append(target+': '+str(rollback))
                state['status']='UNCERTAIN' if errors else 'ERROR';state['error']=str(e);state['rollback_errors']=errors
                try:self._write(tx,state)
                except (OSError,ValueError) as report:print('PATCH failure journal unavailable: '+str(report),flush=True)
                self._ack(tx,state['status']);print('PATCH failure '+json.dumps(state),flush=True)
            finally:
                with self.lock:self.active.discard(tx)
                self.queue.task_done()
                # Keep bounded completed history; never remove unresolved records.
                try:
                    records=sorted(self.root.glob('*.json'),key=lambda p:p.stat().st_mtime)
                    for path in records[:-128]:
                        try:
                            record=json.loads(path.read_text())
                            if record.get('status') in ('OK','ERROR'):path.unlink()
                        except (OSError,ValueError):pass
                except OSError as cleanup:print('PATCH journal cleanup deferred: '+str(cleanup),flush=True)
    @staticmethod
    def _save_audio(path,text):
        import os,uuid
        path.parent.mkdir(parents=True,exist_ok=True);temp=path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
        try:
            with open(temp,'w') as f:f.write(text);f.flush();os.fsync(f.fileno())
            if temp.read_text()!=text:raise OSError('audio save verification failed')
            os.replace(temp,path)
            fd=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
            try:os.fsync(fd)
            finally:os.close(fd)
        finally:temp.unlink(missing_ok=True)
