"""Synthetic UE wire fixtures; no recovered modules or runtime dependencies."""
import base64
import struct

class W:
    def __init__(self): self.b=[]
    def bits(self,v,n): self.b.extend((int(v)>>i)&1 for i in range(n)); return self
    def bit(self,v): return self.bits(v,1)
    def packed(self,v):
        while True:
            part=v&127;v>>=7;self.bits((part<<1)|bool(v),8)
            if not v: return self
    def integer(self,v,maximum):
        decoded=0;mask=1
        while decoded+mask<maximum:
            value=bool(v&mask);self.bit(value)
            if value: decoded|=mask
            mask<<=1
        assert decoded==v
        return self
    def string(self,s):
        raw=s.encode("utf-16-le")+b"\0\0"
        self.bits(-(len(raw)//2)&0xffffffff,32)
        for byte in raw:self.bits(byte,8)
        return self
    def utf8(self,s):
        raw=s.encode("utf-8")+b"\0";self.bits(len(raw),32)
        for byte in raw:self.bits(byte,8)
        return self
    def join(self,other):self.b+=other.b;return self
    def raw(self):
        data=bytearray((len(self.b)+7)//8)
        for i,bit in enumerate(self.b):data[i//8]|=bit<<(i%8)
        return bytes(data)


def pv(w):
    w.integer(0,24)
    for _ in range(3):w.integer(2,4)


def header(w,rng=None):
    w.bits(3,2)
    context=[]
    for _ in range(4):
        n=rng.randrange(32) if rng else 0
        value=rng.getrandbits(n) if rng else 0
        w.bits(n,5).bits(value,n);context.append((n,value<<(32-n) if n else 0))
    return context


def item(w,mask,context,rng=None,pos=(125.4,-678.9,321.0),selector=3):
    w.bits(selector,2)
    if selector==3:w.bits(mask,12)
    if mask&64:w.bits(rng.getrandbits(32-context[3][0]) if rng else 0,32-context[3][0])
    if mask&1:
        for k in range(3):
            q=round(pos[k]*10);zig=(q<<1)^(q>>31)
            n=32-context[k][0];w.bits(rng.getrandbits(n) if rng else zig,n)
        pv(w)
    if mask&128:w.bits(123,8)
    for flag in (256,512,2048,1024):
        if mask&flag:w.bit(1)
    if mask&1:
        w.bits(2,2)
        for x in (0,64,128):w.bit(bool(x));w.bits(x,8) if x else None
    if mask&2:
        w.packed(0x1234).bit(0).string("attachment").bits(0,32).bit(1)
        for x in range(6):w.bits(x*700,32)
        w.bit(0).bit(1)
    if mask&4:
        w.bit(1).bit(1).bits(5,8);pv(w);pv(w);w.bit(0).bits(7,8)
    if mask&8:w.bits(101,32).bit(1).bit(0).bit(1)
    if mask&32:w.bits(0x12345678,32)


def frame(slots=(0,17),rng=None,mask=1,count=None,embedded=False,offset=0,masks=None,embedded_mask=65):
    count=max(slots,default=-1)+1 if count is None else count
    w=W().bits(count,32)
    for word in range((count+31)//32):w.bits(sum(1<<(i%32) for i in slots if i//32==word),32)
    group=-1
    for slot in slots:
        if slot>>4!=group:group=slot>>4;ctx=header(w,rng)
        m=masks.get(slot,mask) if masks else rng.randrange(4096)&~16 if rng else mask
        item(w,m,ctx,rng,pos=(125.4+slot+offset,-678.9,321.0))
    w.bit(0).bit(bool(rng) and count<=64)
    if rng and count<=64:w.bits(rng.getrandbits(count),count)
    w.bit(embedded)
    if embedded:item(w,embedded_mask,header(w,rng),rng,selector=0 if embedded_mask==65 else 3)
    w.bits(0x13579,32)
    return w


def content(payload):return W().bit(1).bit(1).packed(len(payload.b)).join(payload)
def movement_payload(slots=(0,17),mask=1,offset=0):
    return content(W().bit(0).packed(21).join(frame(slots,mask=mask,offset=offset)).packed(0))
def actor_prefix(actor,arch,path,character=False,transforms=False):
    # PackageMap: one class export, then a dynamic actor and its archetype/level.
    w=W().bit(0).bits(1,32).packed(arch).bits(1,8).packed(0).string(path).packed(actor).packed(arch).packed(0)
    if not transforms:
        w.bits(0,4)
        return w.bits(0,8) if character else w
    for field in ("loc","rot","scale","vel"):
        w.bit(1)
        if field=="rot":
            for n in (10000,20000,30000):w.bit(1).bits(n,16)
        else:
            if character:w.bit(field!="scale")
            if character and field=="scale":
                for v in (1.0,1.25,1.5):w.bits(struct.unpack("<I",struct.pack("<f",v))[0],32)
            else:
                w.integer(10,24)
                for v in (50,-100,150):w.bits(v+2048,12)
    return w.bits(0,8) if character else w
def bunch(payload,channel=14,seq=1,opened=False,closed=False,exports=False,partial=None,reliable=True,close_reason=0):
    w=W().bit(opened or closed)
    if opened or closed:
        w.bit(opened).bit(closed)
        if closed:w.integer(close_reason,16)
    w.bit(0).bit(reliable).bit(0).packed(channel).bit(exports).bit(0).bit(partial is not None)
    if reliable:w.integer(seq%1024,1024)
    if partial is not None:w.bit(partial[0]).bit(partial[1])
    if opened or reliable:w.bit(1).packed(102)  # Serialized hardcoded FName; framing is independent of its display name.
    w.integer(len(payload.b),8192).join(payload)
    return w
def packet(parts,seq):
    inner=W().bit(0).bits(0,8)
    for part in parts:inner.join(part)
    inner.bit(1)
    return W().bit(0).bits((seq%16384)<<18,32).bits(0,32).join(inner).bit(1)
def event(w,n,epoch=1):
    raw=w.raw()
    return {"type":"udp_plaintext","session":"ue-test","synthetic":True,"sequence":n,"timestampMs":1700000000000+n*10,
        "data":{"direction":"down","local":"192.0.2.10:1111","remote":"198.51.100.2:2222","epoch":epoch,
        "timestampMs":1700000000000+n*10,"packetOrdinal":n,"structuralValid":True,
        "payloadBase64":base64.b64encode(raw).decode(),"payloadLength":len(raw),"fingerprint":"test-bank"}}

def player_records(count=18):
    char=W().bit(0).packed(14).packed(6).packed(18).packed(6).packed(60).packed(17).packed(0)
    ps=W().bit(0).packed(27).string("测试玩家").packed(62).bits(7,32).packed(0)
    records=[
        event(packet([bunch(W().bits(1,8).string("/Game/TestMap"),channel=0)],1),1),
        event(packet([bunch(actor_prefix(6,7,"BP_SOLPlayerState_C").join(content(ps)),channel=4,opened=True,exports=True)],2),2),
        event(packet([bunch(actor_prefix(4,5,"BP_DFMCharacter_C",True,True).join(content(char)),channel=3,opened=True,exports=True)],3),3),
        event(packet([bunch(actor_prefix(2,3,"MovementReplicationActor"),opened=True,exports=True)],4),4),
    ]
    records += [event(packet([bunch(movement_payload(offset=i),seq=i+2)],i+5),i+5) for i in range(count)]
    return records

def quantized(w,values,scale,maximum):
    values=[round(v*scale) for v in values]
    width=next(n for n in range(2,maximum+2) if all(-(1<<(n-1))<=v<(1<<(n-1)) for v in values))
    w.integer(width-2,maximum)
    for v in values:w.bits(v+(1<<(width-1)),width)
    return w

def self_payload(ts=100.0,loc=(10000.1,-20000.2,300.3),yaw=90,pitch=-20,full=True,
                 vel=(0,0,0),accel=(0,0,0),flags=0):
    w=W().bits(0,53).bit(1).bits(struct.unpack('<I',struct.pack('<f',ts))[0],32)
    quantized(w,vel,10,24);w.bits(0,32)
    w.bits(((round(yaw*65536/360)&65535)<<16)|(round(pitch*65536/360)&65535),32)
    quantized(w,loc,100,30);w.bits(0,48)
    if full:quantized(w,accel,100,30)
    w.bits(flags,8)
    return w

def self_event(n,**kwargs):
    e=event(packet([bunch(self_payload(**kwargs),channel=3,reliable=False)],n),n)
    e['data']['direction']='up'
    return e
