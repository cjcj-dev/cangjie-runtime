#!/usr/bin/env zx
// Check the shared targets selected by the normal native build graph.
import assert from 'node:assert/strict';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

const [directory, ...names] = process.argv.slice(2);
assert.ok(directory && names.length, 'usage: shared_library_soname.mjs <directory> <selected DSO basenames...>');
assert.equal(new Set(names).size, names.length);
const selected = new Set(names);
let failures = 0;
for (const name of names) {
    assert.equal(path.basename(name), name);
    const result = spawnSync('readelf', ['--dynamic', '--wide', path.join(directory, name)], {
        encoding: 'utf8',
    });
    // Reader failures are preparation failures, not a missing-SONAME assertion.
    assert.equal(result.status, 0, result.stderr);
    const values = tag => [...result.stdout.matchAll(new RegExp(`\\(${tag}\\).*?\\[([^\\]]*)\\]`, 'g'))]
        .map(match => match[1]);
    const sonames = values('SONAME');
    const needed = values('NEEDED');
    const rpath = values('RPATH');
    const errors = [];
    if (sonames.length !== 1 || sonames[0] !== name) errors.push('SONAME_MUST_MATCH_OUTPUT_BASENAME');
    if (rpath.length !== 1 || rpath[0] !== '$ORIGIN') errors.push('ORIGIN_RPATH_MUST_BE_PRESERVED');
    for (const dependency of needed) {
        if (dependency.includes('libcangjie-std-') && !selected.has(dependency)) {
            errors.push(`STD_NEEDED_NOT_IN_SELECTED_CLOSURE:${dependency}`);
        }
    }
    console.log(JSON.stringify({name, readerRc: result.status, sonames, needed, rpath, errors}));
    failures += errors.length !== 0 ? 1 : 0;
}
console.log(`STD_SONAME_RESULT targets=${names.length} failures=${failures}`);
process.exitCode = failures ? 1 : 0;
