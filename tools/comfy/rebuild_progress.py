"""Read-only local progress display for the selected-render rebuild."""
from pathlib import Path
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import json
import re
import time

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / 'artifacts/belorta-alpha-rebuild'
PAGE = r'''<!doctype html><meta charset="utf-8"><title>Belorta — Alpha rebuild</title>
<style>body{background:#14171c;color:#e6edf3;font:16px system-ui;margin:36px auto;max-width:1120px;padding:0 24px}h1{font-size:28px;margin-bottom:8px}.muted{color:#9caaba}a{color:#87c6ff}.passes{display:flex;gap:20px;margin:28px 0}.pass{background:#232932;padding:18px;border-radius:10px;flex:1}progress{width:100%;height:12px;accent-color:#55c899}table{width:100%;border-collapse:collapse;font-size:14px}th,td{text-align:left;padding:8px;border-bottom:1px solid #303946}.done{color:#62dba6}.active{color:#87c6ff}.error{color:#ff998b}pre{white-space:pre-wrap;background:#090b0e;padding:16px;font:12px Consolas;max-height:180px;overflow:auto}.banner{background:#203343;padding:16px;border-radius:8px;margin:18px 0}</style>
<h1>Belorta · selected renders → clean alpha</h1>
<div class="muted">48 timeline-selected versions · original seeds and settings · full raw caches retained</div>
<div id="summary" class="banner">Connecting…</div><div id="passes" class="passes"></div>
<p class="muted">Preparation handles tracking, references and text. Generation is the long GPU pass. The final pass removes the background, repairs interior green, and validates the alpha files. Earlier retakes are excluded.</p>
<p><a href="http://127.0.0.1:8188" target="_blank">Open ComfyUI</a> · <span id="updated"></span></p>
<details open><summary>Live processing log</summary><pre id="log"></pre></details>
<table><thead><tr><th>Shot</th><th>Selected seed</th><th>Prepare</th><th>Generate</th><th>Alpha finish</th></tr></thead><tbody id="shots"></tbody></table>
<script>
const labels={prepare:'1 · Prepare',generate:'2 · Generate',finish:'3 · Alpha finish'};
function age(sec){return Math.floor(sec/3600)+'h '+Math.floor(sec%3600/60)+'m '+Math.floor(sec%60)+'s'}
async function update(){try{const s=await(await fetch('/status',{cache:'no-store'})).json();
 document.querySelector('#summary').textContent=s.error?'Batch needs attention: '+s.error:s.stage==='installed'?'Complete — all replacements installed and Ultra Key removed':s.stage==='ready-to-install'?'All renders validated — installing replacements':(labels[s.stage]||s.stage)+' · '+(s.active||'Advancing to next shot')+' · elapsed '+age(s.elapsed);
 document.querySelector('#summary').classList.toggle('error',!!s.error);
 document.querySelector('#passes').innerHTML=Object.entries(labels).map(([key,label])=>'<div class="pass">'+label+'<h2>'+s.counts[key]+' / 48</h2><progress max="48" value="'+s.counts[key]+'"></progress></div>').join('');
 const tbody=document.querySelector('#shots');tbody.replaceChildren();for(const row of s.rows){let tr=document.createElement('tr');for(const value of [row.label,row.seed,...row.stages]){let td=document.createElement('td');td.textContent=value;td.className=value==='Done'?'done':value.startsWith('Running')?'active':'';tr.append(td)}tbody.append(tr)}
 document.querySelector('#log').textContent=s.log;document.querySelector('#updated').textContent='Updated '+new Date().toLocaleTimeString();
 }catch(e){document.querySelector('#summary').textContent='Waiting for batch status: '+e.message}}update();setInterval(update,5000);
</script>'''


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/status':
            state = json.loads((RUN / 'state.json').read_text())
            rows = json.loads((ROOT / 'artifacts/alpha-comparison/belorta-alpha-migration-audit.json').read_text())
            result = {'stage': state['stage'], 'elapsed': time.time() - state['started'],
                      'counts': {}, 'active': '', 'rows': [], 'error': ''}
            if (RUN / 'error.txt').exists():
                result['error'] = (RUN / 'error.txt').read_text().splitlines()[-1]
            for stage in ('prepare', 'generate', 'finish'):
                result['counts'][stage] = sum(v.get('complete', False) for k, v in state['progress'].items() if k.endswith('-' + stage))
            for row in rows:
                statuses = []
                for stage in ('prepare', 'generate', 'finish'):
                    p = state['progress'].get(row['label'][:3] + '-' + stage, {})
                    statuses.append('Done' if p.get('complete') else ('Running · ' + age(time.time()-p['started'])) if p else 'Waiting')
                    if p and not p.get('complete'):
                        result['active'] = row['label']
                result['rows'].append({'label': row['label'], 'seed': str(row['seed'][0]), 'stages': statuses})
            path = ROOT / 'artifacts/alpha-comparison/comfy-alpha-server-error.log'
            with path.open('rb') as f:
                f.seek(max(0, path.stat().st_size - 10000))
                tail = f.read().decode('utf-8', errors='replace')
            result['log'] = '\n'.join(re.sub(r'\x1b\[[0-9;]*m', '', tail).splitlines()[-10:])
            data, kind = json.dumps(result).encode(), 'application/json'
        else:
            data, kind = PAGE.encode(), 'text/html; charset=utf-8'
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)


def age(seconds):
    return f'{int(seconds)//60}m {int(seconds)%60}s'


ThreadingHTTPServer(('127.0.0.1', 8190), Handler).serve_forever()
