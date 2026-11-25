import struct, math
from simulator.api import Node,lib,snapshot

def monitor():return snapshot()
def image(rgb):
    if len(rgb)%3:raise ValueError('RGB incompleto')
    out=bytearray(); workers=[]
    for i in range(0,len(rgb),159):
        r=Node((i//159)%lib.count()).run(3,rgb[i:i+159]);
        if r['status']:raise RuntimeError('Fragmento fallido')
        out.extend(r['data']);workers.append(r['executor'])
    return bytes(out),workers

def regression(points):
    sums=[0.]*5; workers=[]
    for i in range(0,len(points),20):
        block=points[i:i+20]; payload=b''.join(struct.pack('<ff',*p) for p in block)
        r=Node((i//20)%lib.count()).run(4,payload)
        if r['status']:raise RuntimeError('Fragmento fallido')
        sums=[a+b for a,b in zip(sums,struct.unpack('<5d',r['data']))];workers.append(r['executor'])
    n,x,y,xx,xy=sums; den=n*xx-x*x
    if not n or abs(den)<1e-12:raise ValueError('Datos insuficientes o X constante')
    m=(n*xy-x*y)/den;b=(y-m*x)/n
    return dict(slope=m,intercept=b,workers=workers,mse=sum((v-m*u-b)**2 for u,v in points)/len(points))
