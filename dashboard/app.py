from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
from pathlib import Path
import json,sys,base64
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from simulator.api import *
from applications.apps import image,regression
reset()
class Handler(BaseHTTPRequestHandler):
 def reply(self,obj,status=200):
    b=json.dumps(obj).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.end_headers();self.wfile.write(b)
 def do_GET(self):
    if self.path=='/':
      self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.end_headers();self.wfile.write(Path(__file__).with_name('index.html').read_bytes())
    else:self.reply(snapshot())
 def do_POST(self):
    try:
      d=json.loads(self.rfile.read(int(self.headers['Content-Length'])));a=d['action']
      with lock:
       if a=='reset':reset(int(d['n']),1,int(d['loss']),int(d['strategy']))
       elif a=='tick':lib.advance(2000)
       elif a=='toggle':lib.toggle(int(d['i']),int(d['on']));lib.advance(6500)
       elif a=='task':
        r=Node(int(d.get('origin',0))).run(1,bytes([int(d.get('duration',10))]));r['data']=r['data'].hex();return self.reply(r)
       elif a=='ai':return self.reply(regression(d['points']))
       elif a=='image':
        out,w=image(base64.b64decode(d['rgb']));return self.reply({'gray':base64.b64encode(out).decode(),'workers':w})
       elif a=='failure':
        reset();h=Node().submit(1,bytes([100]));lib.advance(100);lib.toggle(2,0);r=Node().result(h);r['data']=r['data'].hex();return self.reply(r)
       else:raise ValueError('Acción desconocida')
       self.reply(snapshot())
    except Exception as e:self.reply({'error':str(e)},400)
if __name__=='__main__':
 print('ComplexNet: http://127.0.0.1:8000');ThreadingHTTPServer(('127.0.0.1',8000),Handler).serve_forever()
