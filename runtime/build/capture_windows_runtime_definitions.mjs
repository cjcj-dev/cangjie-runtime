#!/usr/bin/env zx
// Read the actual normal CMake runtime objects; do not compile a second source set.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {spawnSync} from 'node:child_process';
const [buildArg,sourceArg,revision,outputArg,readobj='llvm-readobj']=process.argv.slice(2);
if(process.platform!=='win32')throw Error('capture requires the actual native Windows build');
if(!outputArg||!/^[a-f0-9]{40}$/.test(revision))throw Error('usage: capture_windows_runtime_definitions.mjs BUILD RUNTIME_SRC REVISION OUTPUT [LLVM_READOBJ]');
const build=path.resolve(buildArg),source=path.resolve(sourceArg),output=path.resolve(outputArg);
const sha=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const db=path.join(build,'compile_commands.json'),commands=JSON.parse(fs.readFileSync(db));
const evidence=output+'.commands';fs.mkdirSync(evidence,{recursive:true});
function read(label,args){const p=spawnSync(readobj,args,{encoding:'utf8',maxBuffer:64*1024*1024});fs.writeFileSync(path.join(evidence,label+'.log'),(p.stdout||'')+(p.stderr||''));fs.writeFileSync(path.join(evidence,label+'.json'),JSON.stringify({cmd:readobj,args,status:p.status,signal:p.signal,error:p.error?.message},null,2));if(p.status!==0)throw Error(`${label}: readobj rc=${p.status}`);return p.stdout;}
const version=read('version',['--version']);
function symbolBlocks(text){
 const blocks=[];let lines=null,depth=0;
 for(const line of text.split('\n')){
  if(lines===null){if(!/^\s*Symbol\s*\{\s*$/.test(line))continue;lines=[];depth=1;continue;}
  if(/\{\s*$/.test(line))depth++;
  if(/^\s*}\s*$/.test(line))depth--;
  if(depth===0){blocks.push(lines.join('\n'));lines=null;}else lines.push(line);
 }
 if(lines!==null)throw Error('truncated COFF symbol table');
 return blocks;
}
const objects=[],seen=new Set();
for(const entry of commands){
 const file=path.resolve(entry.directory,entry.file),relative=path.relative(source,file);
 if(relative.startsWith('..')||path.isAbsolute(relative))continue;
 // CMake emits either arguments/output or its compiler command string. Only
 // extract the -o operand; never execute or reparse the compilation command.
 const argv=entry.arguments||[],index=argv.indexOf('-o');
 const match=entry.command?.match(/(?:^|\s)-o\s+(?:"([^"]+)"|'([^']+)'|(\S+))/);
 const objName=entry.output||(index>=0?argv[index+1]:match?.[1]||match?.[2]||match?.[3]);
 if(!objName||!/[\\/]cangjie-runtime\.dir[\\/]/.test('/'+objName))continue;
 const object=path.resolve(entry.directory,objName);
 if(seen.has(object))continue;seen.add(object);
 const text=read('object-'+objects.length,['--file-headers','--symbols',object]);
 if(!/Format:\s+COFF-/i.test(text))throw Error(`not a COFF object: ${object}`);
 const defined=[];
 for(const block of symbolBlocks(text)){
  if(!/StorageClass:\s+External\b/.test(block)||/Section:\s+IMAGE_SYM_UNDEFINED\b/.test(block))continue;
  const name=block.match(/^\s*Name:\s*(\S+)\s*$/m)?.[1];
  if(name)defined.push(name);
 }
 objects.push({source:relative.replaceAll('\\','/'),source_sha256:sha(file),object:path.relative(build,object).replaceAll('\\','/'),object_sha256:sha(object),defined:[...new Set(defined)].sort()});
}
if(!objects.length)throw Error('normal CMake database contains no cangjie-runtime COFF objects');
const document={system:'Windows',revision,compile_commands_sha256:sha(db),readobj,version,objects};
fs.writeFileSync(output+'.tmp',JSON.stringify(document,null,2)+'\n');fs.renameSync(output+'.tmp',output);
console.log(`WINDOWS_RUNTIME_DEFINITIONS objects=${objects.length} output=${output}`);
