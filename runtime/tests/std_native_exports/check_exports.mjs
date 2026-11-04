#!/usr/bin/env zx
// Derive native implementation names from the actual installed FFI archives.
import {spawnSync} from 'node:child_process';
const directory=process.argv[2],archives=process.argv[3];
if (!directory || !archives) throw Error('installed runtime-lib and static-lib directories required');
const nativeDefinitions=new Set();
for(const name of ['envFFI','processFFI']) {
    const r=spawnSync('nm',['-g','--defined-only',`${archives}/libcangjie-std-${name}.a`],{encoding:'utf8'});
    if(r.status!==0) { process.stderr.write(r.stderr??'archive nm failed'); process.exit(2); }
    for(const line of r.stdout.split('\n')) {
        const match=line.trim().match(/^[0-9a-fA-F]+\s+[A-Z]\s+(\S+)$/);
        if(match) nativeDefinitions.add(match[1]);
    }
}
if(nativeDefinitions.size===0) throw Error('no actual archive definitions observed');
let failed=false;
for (const name of ['console','env','process']) {
    const file=`${directory}/libcangjie-std-${name}.so`;
    const r=spawnSync('nm',['-D','--defined-only',file],{encoding:'utf8'});
    if (r.status!==0) { process.stderr.write(r.stderr??'nm failed'); process.exit(2); }
    const names=r.stdout.trim().split('\n').map(line=>line.trim().split(/\s+/).at(-1));
    const native=names.filter(symbol=>nativeDefinitions.has(symbol));
    const managed=names.filter(symbol=>/^_C/.test(symbol));
    const ok=native.length===0&&managed.length>0;
    console.log(`R19_EXPORT_ASSERT library=${name} reached=1 archiveDefinitions=${nativeDefinitions.size} private=${native.length} managed=${managed.length} ok=${Number(ok)}`);
    if(native.length) console.log(`R19_PRIVATE_EXPORTS library=${name} names=${native.join(',')}`);
    failed ||= !ok;
}
process.exitCode=failed?19:0;
