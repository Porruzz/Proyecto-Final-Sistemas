import ctypes as c, subprocess, pathlib, sys, json, struct, threading, shutil
ROOT=pathlib.Path(__file__).resolve().parents[1]
CORE=ROOT/'firmware/components/complexnet/core'; HOST=ROOT/'tests/host'
LIB=ROOT/'simulator'/('complexnet.dll' if sys.platform=='win32' else 'libcomplexnet.so')
def build():
    if shutil.which('gcc'):
        subprocess.run(['gcc','-shared','-O2','-std=c11',*([] if sys.platform=='win32' else ['-fPIC']),'-I'+str(CORE),'-I'+str(HOST),str(ROOT/'simulator/bridge.c'),str(HOST/'sim_net.c'),*[str(p) for p in CORE.glob('*.c')],'-lm','-o',str(LIB)],check=True)
    elif not LIB.exists():
        raise FileNotFoundError(f'No se encontró gcc en PATH ni la librería precompilada {LIB}')
build(); lib=c.CDLL(str(LIB)); lib.snapshot.restype=c.c_char_p
lib.submit.argtypes=[c.c_int,c.c_int,c.c_char_p,c.c_int]
lock=threading.RLock()
class Node:
    def __init__(self,index=0): self.index=index
    def submit(self,kind,data):
        if len(data)>160: raise ValueError('Máximo 160 bytes')
        with lock:
            start=lib.now(); id=lib.submit(self.index,kind,data,len(data))
            if id<0: raise RuntimeError(f'Error de admisión {id}')
            return id,start
    def result(self,handle):
        id,start=handle; out=c.create_string_buffer(160); meta=(c.c_int*3)()
        with lock:
            for _ in range(400):
                n=lib.result(self.index,id,out,meta)
                if n>=0:return dict(id=id,status=meta[0],executor=meta[1],latency_ms=meta[2]-start,data=out.raw[:n])
                lib.advance(250)
        raise TimeoutError('Sin resultado')
    def run(self,kind,data):
        with lock:
            lib.clear_history(); r=self.result(self.submit(kind,data)); lib.advance(5100); return r

def reset(n=3,seed=1,loss=0,strategy=0):
    if not 3<=n<=6 or not 0<=loss<=50: raise ValueError('Configuración inválida')
    with lock:lib.reset(n,seed,loss,strategy)
def snapshot():
    with lock:return json.loads(lib.snapshot())
