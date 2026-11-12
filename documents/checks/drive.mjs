// Drives an MCP stdio server: initialize, list tools, then the calls given as JSON on argv[3].
import { spawn } from 'node:child_process';
const [server, callsJson] = process.argv.slice(2);
const calls = JSON.parse(callsJson);
const child = spawn('node', [server], { stdio: ['pipe', 'pipe', 'pipe'] });
let buf = ''; const waiters = new Map(); let id = 0;
child.stdout.on('data', (d) => { buf += d; let i; while ((i = buf.indexOf('\n')) >= 0) { const line = buf.slice(0, i); buf = buf.slice(i + 1); if (!line.trim()) continue; let m; try { m = JSON.parse(line); } catch { console.log('NON-JSON ON STDOUT:', line.slice(0, 120)); continue; } if (m.id && waiters.has(m.id)) { waiters.get(m.id)(m); waiters.delete(m.id); } } });
let err = ''; child.stderr.on('data', (d) => { err += d; });
child.on('exit', (c) => { if (waiters.size) { console.log('SERVER EXITED code', c, err.slice(0, 300)); process.exit(1); } });
const rpc = (method, params) => new Promise((res) => { const n = ++id; waiters.set(n, res); setTimeout(() => { if (waiters.has(n)) { waiters.delete(n); res({ error: { message: 'NO ANSWER within ' + (Number(process.env.RPC_TIMEOUT) || 20) + ' s' } }); } }, (Number(process.env.RPC_TIMEOUT) || 20) * 1000); child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: n, method, params }) + '\n'); });
const init = await rpc('initialize', { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'probe', version: '0' } });
console.log('server:', JSON.stringify(init.result?.serverInfo), 'protocol', init.result?.protocolVersion);
child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
const tools = await rpc('tools/list', {});
console.log('tools:', tools.result.tools.map((t) => t.name).join(', '));
for (const c of calls) {
  const t0 = Date.now();
  const r = await rpc('tools/call', { name: c.name, arguments: c.arguments });
  const text = r.result?.content?.[0]?.text ?? JSON.stringify(r.error);
  console.log(`--- ${c.label}: isError=${r.result?.isError ?? false} (${Date.now() - t0} ms, ${text.length} chars)${r.error ? ' ' + r.error.message : ''}`);
  let shown = text;
  try { const j = JSON.parse(text); shown = JSON.stringify({ breakpoints_hit: j.breakpoints_hit, coverage_total: j.coverage_total, hot_path: j.hot_path, full_output_chars: j.full_output?.length }); } catch {}
  console.log(shown.slice(0, 600));
}
child.kill(); process.exit(0);
