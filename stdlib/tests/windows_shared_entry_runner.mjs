#!/usr/bin/env zx
// Only this fixed checkout supplies executable test code. The release payload
// supplies verified products and a data manifest, never scripts to execute.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';

export function validateMember(name, mode = 0) {
    assert(/^[A-Za-z0-9_.-]+(?:\/[A-Za-z0-9_.-]+)*$/.test(name), 'ARCHIVE_PATH');
    for (const part of name.split('/')) {
        assert(part !== '.' && part !== '..' && !part.endsWith('.'), 'ARCHIVE_COMPONENT');
        assert(!/^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(part), 'ARCHIVE_DEVICE');
    }
    assert([0, 0x8000].includes(mode & 0xf000), 'ARCHIVE_NOT_REGULAR');
    assert(name === 'manifest.json' || /\.(dll|exe|a|o)$/.test(name), 'ARCHIVE_NOT_PRODUCT');
    return name;
}

if (process.argv.includes('--self-test')) {
    assert.equal(validateMember('candidate/libcangjie-std-core.dll', 0x81a4), 'candidate/libcangjie-std-core.dll');
    for (const name of ['../x.dll', '/x.dll', 'C:/x.dll', 'a\\x.dll', 'a/../x.dll', 'a/NUL.dll', 'x.dll.', 'run.mjs']) {
        assert.throws(() => validateMember(name), undefined, name);
    }
    assert.throws(() => validateMember('link.dll', 0xa1ff));
    console.log('ARCHIVE_RULES positive=1 rejected=9');
} else {
    assert.equal(process.platform, 'win32');
    assert.equal(process.env.READSCOPE_OTHER_LIFETIME, 'false');
    assert.equal(process.env.READSCOPE_OTHER_PRODUCER, 'false');
    const here = path.dirname(fileURLToPath(import.meta.url)), repo = path.resolve(here, '../..');
    const out = path.join(process.env.RUNNER_TEMP, 'readscope-std-consumer');
    assert(!fs.existsSync(out)); fs.mkdirSync(out);
    const extracted = path.join(out, 'products'); fs.mkdirSync(extracted);
    const expected = process.env.READSCOPE_STD_ASSET_SHA256;
    assert(/^[0-9a-f]{64}$/.test(expected), 'FIXED_ASSET_SHA256');
    const url = new URL(process.env.READSCOPE_STD_ASSET_URL);
    assert.equal(url.origin, 'https://github.com');
    assert(/^\/cjcj-dev\/cangjie-runtime\/releases\/download\/readscope-std-1116-[A-Za-z0-9_.-]+\/readscope-std-1116-[A-Za-z0-9_.-]+\.zip$/.test(url.pathname));
    assert(!url.search && !url.hash && !url.username && !url.password);
    const response = await fetch(url, {signal: AbortSignal.timeout(120000)});
    assert(response.ok, `DOWNLOAD_HTTP_${response.status}`);
    const chunks = []; let total = 0;
    for await (const chunk of response.body) {
        total += chunk.length; assert(total <= 128 * 1024 * 1024, 'ASSET_SIZE_LIMIT'); chunks.push(chunk);
    }
    const data = Buffer.concat(chunks), sha256 = createHash('sha256').update(data).digest('hex');
    fs.writeFileSync(path.join(out, 'download.json'), JSON.stringify({url: url.href, finalUrl: response.url, bytes: total, sha256, expected}, null, 2));
    assert.equal(sha256, expected, 'ASSET_SHA256');
    const archive = path.join(out, 'inputs.zip'); fs.writeFileSync(archive, data);
    const commands = [];
    function run(name, argv, {expectedRc = 0, timeout = 120000, env = process.env} = {}) {
        const started = new Date().toISOString();
        const result = spawnSync(argv[0], argv.slice(1), {cwd: out, env, encoding: 'utf8', timeout, maxBuffer: 16 * 1024 * 1024});
        fs.writeFileSync(path.join(out, name + '.stdout'), result.stdout ?? '');
        fs.writeFileSync(path.join(out, name + '.stderr'), result.stderr ?? '');
        commands.push({name, argv, cwd: out, started, ended: new Date().toISOString(), rc: result.status,
            signal: result.signal, error: result.error?.message ?? null});
        fs.writeFileSync(path.join(out, 'commands.json'), JSON.stringify(commands, null, 2));
        assert.equal(result.status, expectedRc, name + ': ' + result.stderr);
        assert.equal(result.signal, null, name); assert(!result.error, name);
        return result.stdout;
    }
    // Read central-directory metadata before extracting any entry. The second
    // pass only writes the exact validated regular-file allowlist with CreateNew.
    const zipEnv = {...process.env, STD_ZIP: archive, STD_OUT: extracted};
    const listing = run('archive-list', ['pwsh', '-NoProfile', '-Command', `
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z=[IO.Compression.ZipFile]::OpenRead($env:STD_ZIP)
try { @($z.Entries | ForEach-Object { @{name=$_.FullName;size=$_.Length;mode=(($_.ExternalAttributes -shr 16) -band 65535)} }) | ConvertTo-Json -Compress } finally {$z.Dispose()}
`], {env: zipEnv});
    const entries = JSON.parse(listing); assert(Array.isArray(entries) && entries.length > 0 && entries.length <= 128);
    const seen = new Set(); let expanded = 0;
    for (const entry of entries) {
        validateMember(entry.name, entry.mode); const folded = entry.name.toLowerCase();
        assert(!seen.has(folded), 'ARCHIVE_DUPLICATE'); seen.add(folded);
        assert(Number.isSafeInteger(entry.size) && entry.size >= 0); expanded += entry.size;
        assert(expanded <= 512 * 1024 * 1024, 'ARCHIVE_EXPANDED_LIMIT');
    }
    assert(seen.has('manifest.json'));
    fs.writeFileSync(path.join(out, 'archive-entries.json'), JSON.stringify(entries, null, 2));
    run('archive-extract', ['pwsh', '-NoProfile', '-Command', `
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z=[IO.Compression.ZipFile]::OpenRead($env:STD_ZIP)
try { foreach($e in $z.Entries) {
 $p=[IO.Path]::Combine($env:STD_OUT,$e.FullName.Replace('/',[IO.Path]::DirectorySeparatorChar))
 [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($p)) | Out-Null
 $zipInput=$e.Open();$zipOutput=[IO.File]::Open($p,[IO.FileMode]::CreateNew)
 try {$zipInput.CopyTo($zipOutput)} finally {$zipOutput.Dispose();$zipInput.Dispose()}
} } finally {$z.Dispose()}
`], {env: zipEnv});
    const manifest = JSON.parse(fs.readFileSync(path.join(extracted, 'manifest.json')));
    const fileHash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
    assert.equal(manifest.runtimeCommit, '550a133e766020273e3fb7b1220cdd770573c491');
    assert.equal(manifest.compilerCommit, 'a3bd5dc2c1372c34f35aef69279934fc72ff4fea');
    assert.equal(manifest.stdCommit, '8b7e0f88a1746eb656a046883d435fc8f5ee4146');
    assert.equal(manifest.headerSha256, fileHash(path.join(repo, 'runtime/src/Cangjie.h')));
    assert.deepEqual(Object.keys(manifest.files).sort(), entries.map(e => e.name).filter(n => n !== 'manifest.json').sort());
    for (const [name, hash] of Object.entries(manifest.files)) {
        validateMember(name); assert(/^[0-9a-f]{64}$/.test(hash)); assert.equal(fileHash(path.join(extracted, name)), hash, name);
    }
    const bin = process.env.READSCOPE_CLANG64, tool = name => path.join(bin, name + '.exe');
    run('compiler-version', [tool('clang++'), '--version']);
    const observer = path.join(out, 'observer.exe');
    run('observer-build', [process.env.SCCACHE_PATH, tool('clang++'), '-std=c++17', '-O2', '-g', '-static',
        '-I' + path.join(repo, 'runtime/src'), path.join(here, 'windows_shared_entry_consumer.cpp'),
        path.join(extracted, 'common/libcangjie-runtime.dll.a'), '-o', observer]);
    const observations = [];
    for (const arm of ['candidate', 'cut', 'restored']) {
        const directory = path.join(extracted, arm), std = path.join(directory, 'libcangjie-std-core.dll');
        const headers = run(arm + '-pe', [tool('llvm-readobj'), '--file-headers', '--coff-imports', '--coff-exports', std]);
        const entry = Number(headers.match(/AddressOfEntryPoint: (0x[0-9a-f]+)/i)?.[1]);
        const wrapper = Number(headers.match(/Export \{[^}]*Name: __CJ_DllMainCRTStartupV1\s+RVA: (0x[0-9a-f]+)/i)?.[1]);
        assert(Number.isFinite(entry) && Number.isFinite(wrapper)); assert.equal(entry === wrapper, arm !== 'cut');
        const appEnv = {...process.env, PATH: [directory, path.join(extracted, 'common'), bin, process.env.PATH].join(';')};
        const stdout = run(arm + '-consumer', [observer, std, path.join(directory, 'public-image.dll')],
            {expectedRc: arm === 'cut' ? 1 : 0, timeout: 60000, env: appEnv});
        for (const control of ['std-native-load-control', 'std-native-identity-control'])
            assert(stdout.includes(`TARGET ${control} executed=1 pass=1`), control);
        assert(stdout.includes(`TARGET std-public-commit executed=1 pass=${arm === 'cut' ? 0 : 1}`));
        if (arm !== 'cut') assert(stdout.includes('TARGET public-cjc-dll-result executed=1 pass=1'));
        const mapped = [...stdout.matchAll(/^MODULE base=\S+ size=\d+ path=(.+)\r?$/gm)]
            .map(match => path.resolve(match[1].trim()).toLowerCase());
        for (const file of [std, path.join(extracted, 'common/libcangjie-runtime.dll'),
            ...(arm === 'cut' ? [] : [path.join(directory, 'public-image.dll')])]) {
            assert(mapped.includes(path.resolve(file).toLowerCase()), 'ACTUAL_MODULE_IDENTITY ' + file);
        }
        observations.push({arm, entry, wrapper, targetExecuted: true, expectedRc: arm === 'cut' ? 1 : 0});
        fs.writeFileSync(path.join(out, 'results.json'), JSON.stringify(observations, null, 2));
    }
    assert.equal(manifest.files['candidate/libcangjie-std-core.dll'], manifest.files['restored/libcangjie-std-core.dll']);
    console.log('NORMAL_STD_WINDOWS_CONSUMER candidate=0 cut=1 restored=0');
}
