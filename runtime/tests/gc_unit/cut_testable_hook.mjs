#!/usr/bin/env zx
// Negative capability-guard input only. Remove one dynamic definition without
// depending on the optional full symbol table removed by Release installation.
import {readFileSync,writeFileSync} from 'node:fs';
const [path,name]=process.argv.slice(2), b=readFileSync(path);
if(b.subarray(0,4).toString()!=='\x7fELF'||b[4]!==2||b[5]!==1)throw Error('expected little-endian ELF64');
const sectionOffset=Number(b.readBigUInt64LE(40)), sectionSize=b.readUInt16LE(58), sections=b.readUInt16LE(60);
if(sectionSize!==64||sectionOffset+sectionSize*sections>b.length)throw Error('invalid section table');
let found=0;
for(let i=0;i<sections;i++){
 const section=sectionOffset+i*sectionSize;
 if(b.readUInt32LE(section+4)!==11)continue; // SHT_DYNSYM
 const symbols=Number(b.readBigUInt64LE(section+24)), size=Number(b.readBigUInt64LE(section+32));
 const stride=Number(b.readBigUInt64LE(section+56)), link=b.readUInt32LE(section+40);
 if(stride!==24||link>=sections||symbols+size>b.length)throw Error('invalid dynamic symbols');
 const stringsSection=sectionOffset+link*sectionSize;
 const strings=Number(b.readBigUInt64LE(stringsSection+24)), stringsSize=Number(b.readBigUInt64LE(stringsSection+32));
 if(strings+stringsSize>b.length)throw Error('invalid strings');
 for(let symbol=symbols;symbol<symbols+size;symbol+=stride){
  const offset=b.readUInt32LE(symbol), begin=strings+offset, end=b.indexOf(0,begin);
  if(offset>=stringsSize||end<begin||end>=strings+stringsSize)throw Error('invalid symbol name');
  if(b.toString('utf8',begin,end)!==name)continue;
  if(b.readUInt16LE(symbol+6)===0)throw Error('target already undefined');
  b.writeUInt16LE(0,symbol+6); // SHN_UNDEF: no definition is available to a consumer.
  b.writeBigUInt64LE(0n,symbol+8);b.writeBigUInt64LE(0n,symbol+16);found++;
 }
}
if(found!==1)throw Error(`expected one dynamic definition, got ${found}`);
writeFileSync(path,b);
