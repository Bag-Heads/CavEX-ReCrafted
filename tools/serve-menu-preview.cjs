/* Read-only, directory-listing-free design server for Tailscale Serve. */
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const allowed = ['docs/ui_design', 'artwork/cc0-pack'].map(p => fs.realpathSync(path.join(root, p)));
const types = {'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.json':'application/json','.png':'image/png','.jpg':'image/jpeg','.jpeg':'image/jpeg','.svg':'image/svg+xml','.woff':'font/woff','.woff2':'font/woff2','.ttf':'font/ttf','.wav':'audio/wav','.md':'text/plain; charset=utf-8'};
const within = p => allowed.some(base => p === base || p.startsWith(base + path.sep));
http.createServer((req,res)=>{
  if (!['GET','HEAD'].includes(req.method)) {res.writeHead(405);res.end();return;}
  try {
    const url = new URL(req.url, 'http://localhost');
    if(url.pathname === '/') {res.writeHead(302,{Location:'/docs/ui_design/front-end-refined/index.html?phone=1&screen=main'});res.end();return;}
    let file = path.resolve(root, '.' + decodeURIComponent(url.pathname));
    if(!within(file)) {res.writeHead(404);res.end();return;}
    file = fs.realpathSync(file);
    if(!within(file)) {res.writeHead(404);res.end();return;}
    if(fs.statSync(file).isDirectory()) file = fs.realpathSync(path.join(file,'index.html'));
    if(!within(file) || !types[path.extname(file)] || !fs.statSync(file).isFile()) {res.writeHead(404);res.end();return;}
    res.writeHead(200,{'Content-Type':types[path.extname(file)],'Cache-Control':'no-store','X-Content-Type-Options':'nosniff'});
    if(req.method==='HEAD')res.end();else fs.createReadStream(file).on('error',()=>res.destroy()).pipe(res);
  } catch {res.writeHead(404);res.end();}
}).listen(8108,'127.0.0.1',()=>console.log('Menu preview ready on 127.0.0.1:8108'));
