"""Bounded display-name transfer; retry until the controller confirms publication."""
import re,secrets,threading,time,zlib

def encode_list(names,identity):
    if not re.fullmatch('[a-f0-9]{16}',identity):raise ValueError('list identity')
    names=[name.encode('utf-8')[:19] for name in names[:64]]
    if any(b'\x00' in name for name in names):raise ValueError('null display name')
    body=bytes([len(names)])+b''.join(bytes([len(name)])+name for name in names)
    checksum='%08x'%(zlib.crc32(body)&0xffffffff)
    lines=['CLB1:%s:%d:%s'%(identity,len(names),checksum)]
    lines+=['CLI1:%s:%d:%s'%(identity,i,name.hex()) for i,name in enumerate(names)]
    lines+=['CLE1:'+identity]
    assert all(len(line)<80 for line in lines)
    return lines,checksum

class ClipListSender:
    def __init__(self,send,clock=time.monotonic):
        self.send,self.clock=send,clock;self.lock=threading.Lock();self.session=secrets.token_hex(4);self.sequence=0;self.pending=None;self.sent=0.0
    def submit(self,names):
        with self.lock:
            self.sequence+=1;identity=self.session+'%08x'%self.sequence
            lines,checksum=encode_list(names,identity);self.pending=(identity,checksum,lines);self.sent=self.clock()-3
        self.tick()
    def acknowledge(self,line):
        match=re.fullmatch(r'CLA1:([a-f0-9]{16}):([a-f0-9]{8})',line)
        if not match:return False
        with self.lock:
            if self.pending and match.groups()==self.pending[:2]:
                print('CLIP_LIST confirmed '+self.pending[0],flush=True);self.pending=None
        return True
    def tick(self):
        with self.lock:
            if self.pending is None or self.clock()-self.sent<2:return
            batch=self.pending;self.sent=self.clock()
        try:
            for line in batch[2]:
                with self.lock:
                    if self.pending is not batch:return
                self.send(line)
        except OSError as error:print('CLIP_LIST retry pending: '+str(error),flush=True)
    def run(self):
        while True:self.tick();time.sleep(.25)
