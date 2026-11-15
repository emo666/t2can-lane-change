"""Extract actual .ino handlers and execute them against freestanding host fakes.
Run: python tests/persistence_handler_host_test.py [path-to-cl.exe]
No Arduino hardware or actual NVS is exercised.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 't2can_monitor/t2can_monitor.ino').read_text(encoding='utf-8')

def extract(signature):
    start = src.index(signature)
    i = src.index('{', start)
    depth = 0
    state = 'code'
    j = i
    while j < len(src):
        c = src[j]
        n = src[j+1:j+2]
        if state == 'code':
            if c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '/' and n == '/': state = 'line'; j += 1
            elif c == '/' and n == '*': state = 'block'; j += 1
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if not depth: return src[start:j+1]
        elif state in ('string', 'char'):
            if c == '\\': j += 1
            elif (state == 'string' and c == '"') or (state == 'char' and c == "'"): state = 'code'
        elif state == 'line' and c == '\n': state = 'code'
        elif state == 'block' and c == '*' and n == '/': state = 'code'; j += 1
        j += 1
    raise ValueError(signature)

prefix = r'''
typedef unsigned char uint8_t; typedef unsigned int uint32_t;
bool eq(const char* a,const char* b){while(*a && *a==*b){++a;++b;} return *a==*b;}
struct String { const char* p; String(const char* x):p(x){} bool operator==(const char* x)const{return eq(p,x);} bool operator!=(const char* x)const{return !eq(p,x);} String operator+(const char*)const{return *this;} };
struct Server { const char *enable,*highway,*noa; int code; bool hasArg(const char* k){return value(k)!=0;} const char* value(const char* k){return eq(k,"enable")?enable:eq(k,"highway")?highway:noa;} String arg(const char* k){return String(value(k));} void send(int c,const char*,const char*){code=c;} void send(int c,const char*,String){code=c;} } server;
uint32_t now=0; uint32_t millis(){return now;}
bool failBegin=false,failWrite=false; const char* failNamespace=0; int writes=0; bool lastBool=false; int lastPacked=-1; const char* lastNs=0; const char* lastKey=0;
bool fail(const char* ns){return !failNamespace || eq(ns,failNamespace);}
int dbAuto=-1,dbDnd=-1,dbConfig=-1;
struct Preferences {const char* ns; bool begin(const char* n,bool){ns=n;return !(failBegin && fail(n));} int* slot(const char* key){if(eq(ns,"feature-ui")) return eq(key,"autolc")?&dbAuto:&dbDnd; return &dbConfig;} int putBool(const char* key,bool v){++writes; if(failWrite && fail(ns))return 0;lastNs=ns;lastKey=key;lastBool=v;*slot(key)=v;return 1;} int putUChar(const char* key,uint8_t v){++writes;if(failWrite && fail(ns))return 0;lastNs=ns;lastKey=key;lastPacked=v;*slot(key)=v;return 1;} bool getBool(const char* key,bool fallback){int v=*slot(key);return v<0?fallback:v!=0;} uint8_t getUChar(const char* key,uint8_t fallback){int v=*slot(key);return v<0?fallback:(uint8_t)v;} void end(){} };
bool autoLcEnabled=false,autoLcNvsReady=false,dndEnabled=false,dndNvsReady=false,dndCamActive=false,dndCamRestorePending=false; uint32_t dndCamOffMs=0;
struct Cycle { bool enabled; void setEnabled(bool x){enabled=x;} } dndScrollCycle;
bool tlsRefNvsReady=true;
namespace tlssc {struct Config {bool enabled,block_highway,block_noa;}; struct State {bool injected,clear_pending;};}
tlssc::Config tlsRefCfg={false,false,false}; tlssc::State tlsRefState={false,false};
'''
functions = '\n'.join(extract(s) for s in ('static bool saveFeatureFlag(', 'void autoLcHandler(', 'void dndHandler(', 'void tlsRefHandler(', 'static void loadFeatureConfiguration('))
tests = r'''
#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
void args(const char* e=0,const char* h=0,const char* n=0){server.enable=e;server.highway=h;server.noa=n;server.code=0;}
void failures(bool begin,bool write,const char* ns=0){failBegin=begin;failWrite=write;failNamespace=ns;}
void reboot(){autoLcEnabled=false;dndEnabled=false;autoLcNvsReady=false;dndNvsReady=false;dndCamActive=false;dndCamRestorePending=false;dndCamOffMs=0;dndScrollCycle.enabled=false;tlsRefCfg={false,false,false};tlsRefState={false,false};tlsRefNvsReady=false;}
extern "C" int main(){
 args("1"); autoLcHandler(); CHECK(server.code==200 && autoLcEnabled && autoLcNvsReady && lastBool && eq(lastNs,"feature-ui") && eq(lastKey,"autolc"));
 args("0"); autoLcHandler(); CHECK(server.code==200 && !autoLcEnabled && !lastBool);
 failures(false,true); args("1"); autoLcHandler(); CHECK(server.code==500 && !autoLcEnabled && !autoLcNvsReady);
 failures(false,false); args("1"); autoLcHandler(); failures(true,false); args("0"); autoLcHandler(); CHECK(server.code==500 && !autoLcEnabled && !autoLcNvsReady);
 failures(false,false); args("1"); dndHandler(); CHECK(server.code==200 && dndEnabled && dndScrollCycle.enabled && dndNvsReady && eq(lastNs,"feature-ui") && eq(lastKey,"dnd"));
 args("0"); dndHandler(); CHECK(server.code==200 && !dndEnabled && !dndScrollCycle.enabled);
 failures(false,true); args("1"); dndHandler(); CHECK(server.code==500 && !dndEnabled && !dndScrollCycle.enabled && !dndNvsReady);
 failures(false,false); args("1"); dndHandler(); dndCamActive=true; now=100; failures(false,true); args("0"); dndHandler(); CHECK(server.code==500 && !dndEnabled && !dndScrollCycle.enabled && dndCamRestorePending && dndCamOffMs==100);
 now=200; args("0"); dndHandler(); CHECK(server.code==500 && dndCamOffMs==100);
 failures(false,false); dbAuto=dbDnd=dbConfig=-1; reboot(); loadFeatureConfiguration();
 CHECK(!autoLcEnabled && !dndEnabled && !tlsRefCfg.enabled && !tlsRefCfg.block_highway && !tlsRefCfg.block_noa && autoLcNvsReady && dndNvsReady && tlsRefNvsReady);
 args("1"); autoLcHandler(); CHECK(server.code==200); args("1"); dndHandler(); CHECK(server.code==200);
 args("1","1","1"); tlsRefHandler(); CHECK(server.code==200 && tlsRefCfg.enabled && tlsRefCfg.block_highway && tlsRefCfg.block_noa && lastPacked==7 && eq(lastNs,"tlssc-ref") && eq(lastKey,"config"));
 CHECK(dbAuto==1 && dbDnd==1 && dbConfig==7);
 reboot(); loadFeatureConfiguration(); CHECK(autoLcEnabled && dndEnabled && tlsRefCfg.enabled && tlsRefCfg.block_highway && tlsRefCfg.block_noa);
 args(0,"0"); tlsRefHandler(); CHECK(server.code==200 && tlsRefCfg.enabled && !tlsRefCfg.block_highway && tlsRefCfg.block_noa && dbConfig==5);
 args(0,0,"0"); tlsRefHandler(); CHECK(server.code==200 && tlsRefCfg.enabled && !tlsRefCfg.block_highway && !tlsRefCfg.block_noa && dbConfig==1);
 reboot(); loadFeatureConfiguration(); CHECK(tlsRefCfg.enabled && !tlsRefCfg.block_highway && !tlsRefCfg.block_noa);
 args("0"); autoLcHandler(); CHECK(server.code==200); args("0"); dndHandler(); CHECK(server.code==200); args("0"); tlsRefHandler(); CHECK(server.code==200);
 reboot(); loadFeatureConfiguration(); CHECK(!autoLcEnabled && !dndEnabled && !tlsRefCfg.enabled && !tlsRefCfg.block_highway && !tlsRefCfg.block_noa && dbAuto==0 && dbDnd==0 && dbConfig==0);
 failures(true,false,"tlssc-ref"); args("1"); tlsRefHandler(); CHECK(server.code==500 && !tlsRefCfg.enabled && !tlsRefNvsReady && dbConfig==0);
 failures(false,true,"tlssc-ref"); args("1"); tlsRefHandler(); CHECK(server.code==500 && !tlsRefCfg.enabled && !tlsRefNvsReady && dbConfig==0);
 failures(false,false); args("1","1","1"); tlsRefHandler(); CHECK(server.code==200 && dbConfig==7);
 failures(false,true,"tlssc-ref"); args(0,"0"); tlsRefHandler(); CHECK(server.code==500 && tlsRefCfg.enabled && tlsRefCfg.block_highway && tlsRefCfg.block_noa && !tlsRefNvsReady && dbConfig==7);
 tlsRefState.injected=true; args("0"); tlsRefHandler(); CHECK(server.code==500 && !tlsRefCfg.enabled && tlsRefState.clear_pending && !tlsRefNvsReady && dbConfig==7);
 failures(false,false); reboot(); loadFeatureConfiguration(); CHECK(tlsRefCfg.enabled && tlsRefCfg.block_highway && tlsRefCfg.block_noa && tlsRefNvsReady);
 tlsRefState.injected=true; failures(true,false,"tlssc-ref"); args("0"); tlsRefHandler(); CHECK(server.code==500 && !tlsRefCfg.enabled && tlsRefState.clear_pending && !tlsRefNvsReady && dbConfig==7);
 failures(true,false,"tlssc-ref"); reboot(); loadFeatureConfiguration(); CHECK(!tlsRefNvsReady && !tlsRefCfg.enabled && autoLcNvsReady && dndNvsReady);
 failures(true,false,"feature-ui"); reboot(); loadFeatureConfiguration(); CHECK(!autoLcNvsReady && !dndNvsReady && tlsRefNvsReady && tlsRefCfg.enabled);
 failures(false,false); reboot(); loadFeatureConfiguration(); CHECK(autoLcNvsReady && dndNvsReady && tlsRefNvsReady && tlsRefCfg.enabled);
 return 0;
}
'''
cl = sys.argv[1] if len(sys.argv)>1 else r'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64\cl.exe'
with tempfile.TemporaryDirectory() as td:
    tmp = Path(td); cpp = tmp/'handlers.cpp'; exe = tmp/'handlers.exe'
    cpp.write_text(prefix+'\n'+functions+'\n'+tests, encoding='utf-8')
    result = subprocess.run([cl, '/nologo','/utf-8','/EHs-c-','/GS-','/Zl',str(cpp),'/Fe:'+str(exe),'/link','/nodefaultlib','/entry:main','/subsystem:console'], cwd=td)
    if result.returncode: sys.exit(result.returncode)
    result = subprocess.run([str(exe)])
    if result.returncode:
        lines=(prefix+'\n'+functions+'\n'+tests).splitlines(); line=result.returncode
        print(f'FAIL at generated C++ line {line}: {lines[line-1] if line <= len(lines) else "unknown"}', flush=True)
        sys.exit(1)
    print('PASS: extracted .ino handlers; visible feature persistence, DND restore timing and storage failures')
