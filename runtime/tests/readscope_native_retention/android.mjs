#!/usr/bin/env zx
// Official Linux GHA runner: normal Android production, then x64 emulator only.
import fs from 'node:fs';import path from 'node:path';import assert from 'node:assert/strict';import {fileURLToPath} from 'node:url';import {spawnSync,spawn} from 'node:child_process';import {createHash} from 'node:crypto';
assert.equal(process.platform,'linux');
const here=path.dirname(fileURLToPath(import.meta.url)),runtime=path.resolve(here,'../..'),out=path.join(process.env.RUNNER_TEMP,'readscope-android');fs.mkdirSync(out);
const android=process.env.ANDROID_HOME;assert(android&&path.isAbsolute(android));const ndkVersion='27.3.13750724',ndk=path.join(android,'ndk',ndkVersion),toolchain=path.join(ndk,'toolchains/llvm/prebuilt/linux-x86_64'),tools=toolchain+'/bin',adb=android+'/platform-tools/adb';
// Keep the Java AVD producer and native emulator on the same private paths.
// Runner defaults for the SDK/user/emulator homes need not agree.
const androidUser=out+'/.android',avdHome=androidUser+'/avd';fs.mkdirSync(avdHome,{recursive:true});
const env={...process.env,ANDROID_SDK_HOME:out,ANDROID_USER_HOME:androidUser,ANDROID_EMULATOR_HOME:androidUser,ANDROID_AVD_HOME:avdHome,CANGJIE_BUILD_JOBS:'2',CMAKE_BUILD_PARALLEL_LEVEL:'2',GC_UNIT_GATE_SKIP:'1',SCCACHE_GHA_ENABLED:'true',TMPDIR:out};
fs.writeFileSync(out+'/avd-paths.json',JSON.stringify({ANDROID_SDK_HOME:out,ANDROID_USER_HOME:androidUser,ANDROID_EMULATOR_HOME:androidUser,ANDROID_AVD_HOME:avdHome}));
const rows=[],save=()=>fs.writeFileSync(out+'/commands.json',JSON.stringify(rows,null,2));
function run(name,args,{input,expected=0,timeout=2700000}={}){const row={name,args,cwd:out,started:new Date().toISOString(),expected};rows.push(row);save();const a=fs.openSync(out+'/'+name+'.stdout','wx'),b=fs.openSync(out+'/'+name+'.stderr','wx'),r=spawnSync(args[0],args.slice(1),{cwd:out,env,input,stdio:[input===undefined?'ignore':'pipe',a,b],timeout});fs.closeSync(a);fs.closeSync(b);Object.assign(row,{rc:r.status,signal:r.signal,error:r.error?.message,ended:new Date().toISOString()});save();console.log(name,row.rc,row.signal);if(expected!==null){assert.equal(r.status,expected,name);assert.equal(r.signal,null,name);}return fs.readFileSync(out+'/'+name+'.stdout','utf8');}
function files(p){return fs.readdirSync(p,{withFileTypes:true}).flatMap(e=>e.isDirectory()?files(p+'/'+e.name):[p+'/'+e.name]);}
const hash=p=>createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const ref=run('source-ref',['git','-C',path.dirname(runtime),'rev-parse','HEAD']).trim();
// Install only the frozen dependencies, including their ordinary sdkmanager
// license prompts; closed stdin otherwise rejects the system image before build.
run('sdk-packages',[android+'/cmdline-tools/latest/bin/sdkmanager','--install','ndk;'+ndkVersion,'platform-tools','emulator','system-images;android-31;google_apis;x86_64'],{input:'y\n'.repeat(8)});
run('compiler-version',[tools+'/clang','--version']);const built=[];
for(const [arch,flag] of [['x86_64','2'],['aarch64','1']]){
 const build=out+'/'+arch+'/build',install=out+'/'+arch+'/install',cc=tools+'/'+arch+'-linux-android31-clang';
 run(arch+'-configure',['cmake','-S',runtime,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DANDROID_FLAG='+flag,'-DANDROID_ROOT='+ndk+'/toolchains','-DCMAKE_ANDROID_API=31','-DOHOS_FLAG=0','-DWINDOWS_FLAG=0','-DCOPYGC_FLAG=1','-DDOPRA_FLAG=1','-DRUNTIME_TRACE_FLAG=1','-DCJ_SDK_VERSION=0.0.1','-DDISABLE_VERSION_CHECK=1','-DCMAKE_C_COMPILER='+cc,'-DCMAKE_CXX_COMPILER='+cc+'++','-DCMAKE_ASM_COMPILER='+cc,'-DCMAKE_C_COMPILER_LAUNCHER=sccache','-DCMAKE_CXX_COMPILER_LAUNCHER=sccache','-DCMAKE_INSTALL_PREFIX='+install,'-DCJ_RUNTIME_COMMIT='+ref,'-DMRT_GC_UNIT_TESTS=OFF']);
 run(arch+'-build',['cmake','--build',build,'--parallel','2']);run(arch+'-install',['cmake','--install',build]);
 const members=files(install),libraries=members.filter(p=>p.endsWith('/libcangjie-runtime.so')),start=members.filter(p=>p.endsWith('/cjstart.o'));assert.equal(libraries.length,1);assert.equal(start.length,1);
 run(arch+'-startup-relocs',[tools+'/llvm-readelf','-sWr',start[0]]);run(arch+'-startup-machine',[tools+'/llvm-objdump','-dr',start[0]]);
 const machineMembers=['src/ObjectModel/CMakeFiles/ObjectModel.dir/MClass.cpp.o','src/CMakeFiles/cangjie-runtime.dir/StackManager.cpp.o','src/CMakeFiles/runtime.dir/StackManager.cpp.o'];
 fs.writeFileSync(out+'/'+arch+'-machine-members.json',JSON.stringify(machineMembers,null,2));
 for(const [index,member] of machineMembers.entries()){const object=build+'/'+member;assert(fs.statSync(object).isFile(),member);const body=run(arch+'-'+index+'-'+path.basename(object)+'-machine',[tools+'/llvm-objdump','-dr',object]);assert(!/ElfUnloadQuiescence[^\n]*(?:ReadScope|AssertReaderActive|CheckReaderActive)|readerDepth/.test(body),member);}
 built.push({arch,ref,runtime:{path:libraries[0],sha256:hash(libraries[0])},startup:{path:start[0],sha256:hash(start[0])},build,install});
}
fs.writeFileSync(out+'/identity.json',JSON.stringify({ref,ndkVersion,built,scope:'normal two-architecture Android cross production; only x86_64 emulator lifecycle; AArch64 target execution and Debug/O0 NOT_RUN; finite metadata fixture, not generated CJ library/SDK'},null,2));
const x64=built[0],lib=path.dirname(x64.runtime.path),headers=[runtime+'/src',runtime+'/include',runtime+'/src/Loader/BinaryFile',runtime+'/src/CJThread/src/runtime/schedule/include',runtime+'/third_party/third_party_bounds_checking_function/include',...files(x64.build).filter(p=>p.endsWith('/log.h')).map(p=>path.dirname(p))],common=[tools+'/x86_64-linux-android31-clang++','-std=gnu++17','-O2',...headers.map(p=>'-I'+p)];
run('fixture-link',[...common,'-shared','-fPIC','-Wl,-Bsymbolic-functions',here+'/elf-fixture.cpp','-L'+lib,'-lcangjie-runtime','-ldl','-o',out+'/fixture.so']);
run('consumer-link',[...common,here+'/elf-consumer.cpp','-L'+lib,'-lcangjie-runtime','-ldl','-o',out+'/consumer']);
run('create-avd',[android+'/cmdline-tools/latest/bin/avdmanager','create','avd','--name','readscope-lifetime','--path',avdHome+'/readscope-lifetime.avd','--package','system-images;android-31;google_apis;x86_64','--device','pixel_2'],{input:'no\n'});
assert(fs.statSync(avdHome+'/readscope-lifetime.ini').isFile(),'AVD registration exists before emulator launch');
run('list-avds',[android+'/cmdline-tools/latest/bin/avdmanager','list','avd']);
const emulatorAvds=run('emulator-list-avds',[android+'/emulator/emulator','-list-avds']);assert(emulatorAvds.split(/\r?\n/).includes('readscope-lifetime'),'native emulator sees the created AVD');
const log=fs.openSync(out+'/emulator.stdout','wx'),err=fs.openSync(out+'/emulator.stderr','wx'),emulator=spawn(android+'/emulator/emulator',['-avd','readscope-lifetime','-port','5554','-no-window','-no-audio','-no-boot-anim','-no-snapshot','-gpu','swiftshader_indirect','-memory','2048'],{env,stdio:['ignore',log,err]});let killed=false;const emulatorExit=new Promise(resolve=>{emulator.on('error',error=>resolve({error:error.message}));emulator.on('close',(rc,signal)=>resolve({rc,signal}));});
try{
 run('wait-device',[adb,'-s','emulator-5554','wait-for-device'],{timeout:180000});const deadline=Date.now()+240000;let boot=false;
 while(Date.now()<deadline){const r=spawnSync(adb,['-s','emulator-5554','shell','getprop','sys.boot_completed'],{encoding:'utf8',timeout:5000});if(r.status===0&&r.stdout.trim()==='1'){boot=true;break;}await new Promise(resolve=>setTimeout(resolve,2000));}assert(boot,'Android emulator boot deadline');
 run('device-identity',[adb,'-s','emulator-5554','shell','getprop']);run('remote-directory',[adb,'-s','emulator-5554','shell','mkdir','-p','/data/local/tmp/readscope-lifetime']);
 const deploy=[out+'/fixture.so',out+'/consumer',...files(lib).filter(p=>p.endsWith('.so')),toolchain+'/sysroot/usr/lib/x86_64-linux-android/libc++_shared.so'];const names=new Set();for(const [i,p] of deploy.entries()){assert(!names.has(path.basename(p)),'ambiguous native dependency '+p);names.add(path.basename(p));run('push-'+i,[adb,'-s','emulator-5554','push',p,'/data/local/tmp/readscope-lifetime/']);}
 run('consumer-permissions',[adb,'-s','emulator-5554','shell','chmod','700','/data/local/tmp/readscope-lifetime/consumer']);
 for(const mode of ['preinit','runtime-fini']){const output=run(mode,[adb,'-s','emulator-5554','shell','env','LD_LIBRARY_PATH=/data/local/tmp/readscope-lifetime','/data/local/tmp/readscope-lifetime/consumer','/data/local/tmp/readscope-lifetime/fixture.so',mode],{expected:null,timeout:20000}),row=rows.at(-1),stderr=fs.readFileSync(out+'/'+mode+'.stderr','utf8'),combined=output+stderr;assert.equal(row.rc,0,mode);assert.equal(row.signal,null,mode);assert(combined.includes('elf-published-package executed=1 pass=1'));assert.equal((combined.match(/ELF_FIXTURE_INIT count=1/g)||[]).length,1);assert.equal((combined.match(/ELF_FIXTURE_FINI count=1/g)||[]).length,1);}
}finally{
 run('crash-log',[adb,'-s','emulator-5554','logcat','-d','-b','crash'],{expected:null,timeout:10000});run('stop-emulator',[adb,'-s','emulator-5554','emu','kill'],{expected:null,timeout:10000});
 const ended=await Promise.race([emulatorExit,new Promise(resolve=>setTimeout(()=>resolve(null),10000))]);if(ended===null){killed=emulator.kill('SIGTERM');}fs.writeFileSync(out+'/emulator-result.json',JSON.stringify({pid:emulator.pid,ended,explicitTerminated:killed}));fs.closeSync(log);fs.closeSync(err);
}
