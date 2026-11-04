"""Matched C5/C4 retained-binary idle samples; no native input is synthesized."""
import argparse,hashlib,json,os,pathlib,subprocess,time
import psutil
root=pathlib.Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--output',default='docs/phase-2c5/idle-comparison');parser.add_argument('--control-only',action='store_true');options=parser.parse_args()
out=root/options.output;out.mkdir(exist_ok=False)
environment={**os.environ,'PATH':str(root/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
executables={'c5':root/'build/studio-release/src/nova-studio.exe','c4-control':root/'build/studio-c4-acceptance-release/src/nova-studio.exe'}
if options.control_only:executables.pop('c5')
rows=[]
for label,exe in executables.items():
    assert exe.exists(),exe
    for pack in ('empty','pack-1500','pack-1500-hidden'):
        args=[str(exe),'--settings',str(out/(label+'-'+pack+'.ini')),'--fresh-layout','--smoke-seconds','35']
        if pack!='empty':args+=['--project',str(root/'fixtures/generated'/pack.removesuffix('-hidden'))]
        if pack.endswith('-hidden'):args+=['--no-preview']
        child=subprocess.Popen(args,cwd=root,env=environment,creationflags=subprocess.CREATE_NO_WINDOW,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        process=psutil.Process(child.pid);time.sleep(10);start=time.perf_counter();before=process.cpu_times();samples=[]
        for _ in range(4):
            time.sleep(5);cpu=process.cpu_times();memory=process.memory_info();seconds=time.perf_counter()-start
            samples.append({'seconds':seconds,'cpu_ms':1000*(cpu.user+cpu.system-before.user-before.system),'working_set_bytes':memory.rss,'private_bytes':getattr(memory,'private',0),'threads':process.num_threads()})
        rows.append({'build':label,'pack':pack,'exe':str(exe),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'cpu_percent_one_processor':samples[-1]['cpu_ms']/samples[-1]['seconds']/10,'samples':samples})
        child.wait(timeout=15)
        (out/'summary.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'samples':rows,'scope':'Sequential samples after 10 s warmup, measured over 20 s. C4 retained-binary source provenance is not re-certified; executable identity is hashed.'},indent=2)+'\n')
print(json.dumps(rows,indent=2))
