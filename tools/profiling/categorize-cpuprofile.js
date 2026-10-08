// categorize-cpuprofile.js FILE.cpuprofile [LAST_SECONDS]: buckets CPU-worker self time
// (tools/profiling/README.md), optionally only over the profile's last LAST_SECONDS.
const p=JSON.parse(require("fs").readFileSync(process.argv[2]));
const lastUs=process.argv[3]?Number(process.argv[3])*1e6:Infinity;
const byId=new Map(p.nodes.map(n=>[n.id,n]));
const self=new Map(); let total=0;
const endUs=p.timeDeltas.reduce((a,b)=>a+b,0); let t=0;
const isCore=url=>url.includes("switch_core")||/voland-cli\.(js|wasm)/.test(url);
for(let i=0;i<p.samples.length;i++){const d=p.timeDeltas[i]||0; t+=d; if(endUs-t>lastUs) continue; const n=byId.get(p.samples[i]); const cf=n.callFrame; const k=cf.functionName+"|"+(cf.url.startsWith("wasm://")&&!/^\$?[a-z_]{3,}/i.test(cf.functionName.replace(/^wasm-function\[\d+\]$/,""))?"jit":cf.url.startsWith("wasm://")||isCore(cf.url)?"core":cf.url); self.set(k,(self.get(k)||0)+d); total+=d;}
const cat=(name,src)=>{
  if(src==="jit") return "JIT-compiled guest code";
  if(src!=="core") return name.startsWith("(")?"idle/program/GC: "+name:"JS (worker glue)";
  if(/^jit_/.test(name)) return "JIT helpers";
  if(/^(interp_|fp_|simd|neon|exec_|decode_|predecode|round_value|u128|softfloat|f32_|f64_|f16_|sf_|norm|pack|propagate|shift_right_jam|add_|sub_|mul_|div_|sqrt|fused)/.test(name)) return "interpreter fallback (+softfloat)";
  if(/^(sm_|run_vertices|gpu_probe_texture|tex_fetch|sample_|tex_sample|vertex_shader|ps_|vs_)/.test(name)) return "Maxwell shader interp (sm_run, vertex programs, probe)";
  if(/^(raster|emit_triangle|vertex_prefetch|gpu_put_vertex|gpu_|clip|draw_|setup_|texture_|tex_|astc|block_linear|wgsl|surface|attr_|fetch_|index_|primitive|Draw|maxwell)/.test(name)) return "raster3d producer / textures";
  if(/^(hle_|ipc_|svc_|nvdrv|sm_registry|service|kernel|sched|thread|event|vmm_|handle|hid|vi_|audren|audout|fs_|am_|time_|syncpoint|gpu_channel|channel)/.test(name)) return "HLE/IPC/nvdrv/vmm/scheduler";
  if(/^(memcpy|memset|memmove|emscripten|__|dlmalloc|malloc|free|strlen)/.test(name)) return "libc (memcpy/memset)";
  return "other core: "+name;
};
const cats=new Map(); const members=new Map();
for(const [k,v] of self){const [n,s]=k.split("|"); const c=cat(n,s); cats.set(c,(cats.get(c)||0)+v); if(!members.has(c)) members.set(c,[]); members.get(c).push([n,v]);}
const order=[...cats.entries()].sort((a,b)=>b[1]-a[1]);
console.log("total",(total/1e3).toFixed(0),"ms");
for(const [c,v] of order){ if(c.startsWith("other core")||c.startsWith("idle")) continue; const top=members.get(c).sort((a,b)=>b[1]-a[1]).slice(0,6).map(([n,x])=>n.replace("wasm-function[5]","jit")+" "+(100*x/total).toFixed(1)).join(", "); console.log((100*v/total).toFixed(1).padStart(5)+"%  "+c.padEnd(52)+top); }
let other=0, idle=0; const otherList=[];
for(const [c,v] of order){ if(c.startsWith("other core")){other+=v; otherList.push([c.slice(12),v]);} if(c.startsWith("idle")) {idle+=v; console.log((100*v/total).toFixed(1).padStart(5)+"%  "+c);} }
console.log((100*other/total).toFixed(1).padStart(5)+"%  other core: "+otherList.sort((a,b)=>b[1]-a[1]).slice(0,14).map(([n,v])=>n+" "+(100*v/total).toFixed(1)).join(", "));
if(process.env.TOP){ console.log("top functions:"); [...self.entries()].sort((a,b)=>b[1]-a[1]).slice(0,Number(process.env.TOP)).forEach(([k,v])=>console.log((100*v/total).toFixed(2).padStart(6)+"%  "+k)); }
