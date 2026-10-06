// Use the fixed producer's canonical formatter, including truthful absent rows.
import fs from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
const [repo, root, output, baseSdkSha256] = process.argv.slice(2);
const canonical = await import(pathToFileURL(path.join(repo, 'ci/llvm-tools-manifest.mjs')));
const tuple = canonical.parseLlvmToolsManifest(await fs.readFile(path.join(output, 'llvm-tools.manifest'), 'utf8')).values;
const names = [...canonical.PACKAGED_LLVM_TOOL_NAMES, 'llvm-readobj'].sort((a, b) => a.localeCompare(b));
const available = new Set(['llc', 'opt', tuple.get('LLD_TOOL'), 'llvm-readobj']);
const tools = [];
for (const tool of names) {
  if (!available.has(tool)) {
    tools.push({tool, present: 'no', source: 'none', version: '-', sha256: '-'});
    continue;
  }
  const binary = path.join(root, 'llvm-build/bin', tool + (process.platform === 'win32' ? '.exe' : ''));
  const version = execFileSync(binary, ['--version'], {encoding: 'utf8'}).split(/\r?\n/).find(line => /LLVM version |^LLD /.test(line))?.trim();
  if (!version) throw new Error('missing LLVM tool version: ' + tool);
  tools.push({tool, present: 'yes', source: 'tuple:' + tuple.get('LLVM_SHA'), version,
              sha256: createHash('sha256').update(await fs.readFile(binary)).digest('hex')});
}
const manifest = canonical.formatPackagedLlvmToolsManifest({llvmSha: tuple.get('LLVM_SHA'), baseSdkSha256, tools});
canonical.parsePackagedLlvmToolsManifest(manifest);
await fs.writeFile(path.join(output, 'llvm-tools.packaged.manifest'), manifest);
