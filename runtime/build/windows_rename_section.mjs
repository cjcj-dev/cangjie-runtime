#!/usr/bin/env zx
// Preserve COFF subsection suffixes while keeping the native command line short.
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

function run(tool, args, capture = false) {
    console.log(`WINDOWS_RUNTIME_POSTPROCESS ${JSON.stringify({tool, args})}`);
    const result = spawnSync(tool, args, {
        encoding: 'utf8',
        stdio: ['ignore', capture ? 'pipe' : 'inherit', 'inherit'],
        maxBuffer: 128 * 1024 * 1024,
    });
    if (result.error || result.signal || result.status !== 0) {
        const error = new Error(`${tool}: rc=${result.status} signal=${result.signal} ${result.error?.message || ''}`);
        error.exitCode = result.status > 0 && result.status < 256 ? result.status : 1;
        throw error;
    }
    return result.stdout;
}

function sections(objdump, file) {
    const output = run(objdump, ['-h', file], true);
    const found = [...output.matchAll(/^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+2\*\*(\d+)/gm)]
        .map(match => ({name: match[1], size: Number.parseInt(match[2], 16), alignment: 2 ** Number(match[3])}));
    if (!found.length) throw new Error(`no archive sections decoded: ${file}`);
    return found;
}

const isRuntimeText = name => name.startsWith('.text_rt');
const needsRename = name => name.startsWith('.text') && !isRuntimeText(name);
const renamed = name => needsRename(name) ? '.text_rt' + name.slice(5) : name;
// BFD aligns COFF raw section sizes when writing an object. Compare the
// declared aligned extent and alignment, not the compiler's unpadded size.
const signature = list => list.map(({name, size, alignment}) =>
    `${name}\t${Math.ceil(size / alignment) * alignment}\t${alignment}`).sort();
// GNU response-file quoting: no shell expansion, including '$' in COFF names.
const quote = arg => '"' + arg.replaceAll('\\', '\\\\').replaceAll('"', '\\"') + '"';

try {
    const [objdump, objcopy, input, ...extra] = process.argv.slice(2);
    if (!objdump || !objcopy || !input || extra.length)
        throw new Error('usage: windows_rename_section.mjs <objdump> <objcopy> <archive>');
    const file = path.resolve(input);
    run(objdump, ['--version']);
    run(objcopy, ['--version']);
    const before = sections(objdump, file);
    const names = [...new Set(before.filter(({name}) => needsRename(name)).map(({name}) => name))];
    if (!names.length) {
        if (!before.some(({name}) => isRuntimeText(name))) throw new Error(`archive has no runtime text: ${file}`);
        console.log(`WINDOWS_RUNTIME_SECTIONS_ALREADY_RENAMED ${file}`);
    } else {
        const directory = fs.mkdtempSync(path.join(path.dirname(file), '.runtime-sections-'));
        const output = path.join(directory, 'runtime.a');
        const response = path.join(directory, 'objcopy.rsp');
        const args = ['-D', ...names.flatMap(name => ['--rename-section', `${name}=${renamed(name)}`]), file, output];
        fs.writeFileSync(response, args.map(quote).join('\n') + '\n');
        // On failure retain the response/output beside the installation for diagnostics.
        run(objcopy, ['@' + response]);
        const after = sections(objdump, output);
        const expected = signature(before.map(section => ({...section, name: renamed(section.name)})));
        if (JSON.stringify(signature(after)) !== JSON.stringify(expected))
            throw new Error(`archive section rename/size verification failed: ${output}`);
        fs.renameSync(output, file);
        fs.rmSync(directory, {recursive: true});
        console.log(`WINDOWS_RUNTIME_SECTIONS_RENAMED unique=${names.length} sections=${before.length} archive=${file}`);
    }
} catch (error) {
    console.error(error.stack);
    process.exitCode = error.exitCode || 1;
}
