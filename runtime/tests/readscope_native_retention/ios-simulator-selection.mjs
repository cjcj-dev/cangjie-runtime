// Resolve the runtime from the selected SDK and its actual device membership.
// Do not silently substitute the newest installed runtime or an arbitrary phone.
import assert from 'node:assert/strict';
export function selectSimulator({sdkVersion,arch,runtimes,types,expected={}}){
 if(expected.sdkVersion)assert.equal(sdkVersion,expected.sdkVersion,'frozen simulator SDK');
 const eligible=runtimes.filter(r=>r.isAvailable&&r.identifier.includes('.iOS-')&&r.version===sdkVersion&&r.supportedArchitectures?.includes(arch)&&(!expected.runtime||r.identifier===expected.runtime));
 assert.equal(eligible.length,1,'one available native-architecture simulator matching the selected SDK');
 const runtime=eligible[0];if(expected.build)assert.equal(runtime.buildversion,expected.build,'frozen simulator runtime build');
 const supported=new Set(runtime.supportedDeviceTypes?.map(d=>d.identifier));
 const devices=types.filter(d=>d.productFamily==='iPhone'&&supported.has(d.identifier)&&(!expected.device||d.identifier===expected.device)).sort((a,b)=>a.identifier.localeCompare(b.identifier));
 assert(devices.length,'device must belong to the selected runtime');
 return {runtime,deviceType:devices[0]};
}
