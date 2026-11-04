"""Runs native executable QA modes and core benchmarks. No fixture PNGs are changed."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
ENV=os.environ.copy()
ENV['PATH']=str(ROOT/'.tools/qt/bin')+os.pathsep+ENV['PATH']
EXES={'rust':ROOT/'build/nova-egui.exe','qt':ROOT/'build/qt-review/nova-qt.exe'}

def run(command, timeout=60):
    result=subprocess.run([str(a) for a in command],cwd=ROOT,env=ENV,capture_output=True,timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{command}: exit {result.returncode}: {result.stderr.decode(errors="replace")}')
    return result

def qa():
    for name,exe in EXES.items():
        out=ROOT/'docs/phase-1/screenshots'/name
        out.mkdir(parents=True,exist_ok=True)
        result=run([exe,'--project','fixtures/generated/pack-1500','--fresh-layout','--qa-dir',out,'--telemetry',ROOT/f'benchmarks/{name}-qa.json'],30)
        print(name,'QA',len(list(out.glob('*.png'))),'captures',result.stderr.decode(errors='replace')[:1000],flush=True)

def core():
    out=ROOT/'docs/benchmarks/raw'
    out.mkdir(parents=True,exist_ok=True)
    for count in (1500,10000,50000):
        # Alternate order across repetitions; all runs use OS-warm filesystem caches.
        for repeat in range(3):
            order=list(EXES.items())
            if repeat%2:order.reverse()
            for name,exe in order:
                run([exe,'--bench',ROOT/f'fixtures/generated/pack-{count}','--output',out/f'{name}-{count}-{repeat}.json'])
                print(name,count,repeat,'complete',flush=True)

def exercise():
    out=ROOT/'docs/benchmarks/raw'
    out.mkdir(parents=True,exist_ok=True)
    for name,exe in EXES.items():
        run([exe,'--project','fixtures/generated/pack-10000','--fresh-layout','--exercise','--telemetry',out/f'{name}-exercise.json'],30)
        print(name,'native exercise complete',flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('mode',choices=('qa','core','exercise'))
    globals()[parser.parse_args().mode]()
