#!/usr/bin/env zx
// Consume the normal CMake link graph and its already compiled COFF objects.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const sha = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const key = file => process.platform === 'win32' ? path.resolve(file).toLowerCase() : path.resolve(file);
const within = (root, file) => {
    const relative = path.relative(root, file);
    return relative !== '..' && !relative.startsWith('..' + path.sep) && !path.isAbsolute(relative);
};
const portable = file => file.replaceAll('\\', '/');

function outputObject(entry) {
    const index = entry.arguments?.indexOf('-o') ?? -1;
    const match = entry.command?.match(/(?:^|\s)-o\s+(?:"([^"]+)"|'([^']+)'|(\S+))/);
    const name = entry.output || (index >= 0 ? entry.arguments[index + 1] : match?.[1] || match?.[2] || match?.[3]);
    return name ? path.resolve(entry.directory, name) : null;
}

// Exported for graph-parser fixtures. This does not run the native capture or
// allow a fixture to claim Windows COFF qualification.
export function planRuntimeObjects(build, source) {
    build = path.resolve(build); source = path.resolve(source);
    const reply = path.join(build, '.cmake/api/v1/reply');
    const indexes = fs.readdirSync(reply).filter(name => /^index-.*\.json$/.test(name)).sort();
    if (!indexes.length) throw Error('normal CMake file-api index is missing');
    const indexFile = path.join(reply, indexes.at(-1));
    const index = JSON.parse(fs.readFileSync(indexFile));
    const entry = index.reply?.['client-runtime-exports']?.['codemodel-v2'];
    if (!entry?.jsonFile) throw Error('normal runtime codemodel-v2 reply is missing');
    const modelFile = path.join(reply, entry.jsonFile);
    const model = JSON.parse(fs.readFileSync(modelFile));
    if (key(model.paths.build) !== key(build) || model.configurations.length !== 1)
        throw Error('runtime capture requires the matching single-configuration CMake graph');
    const modelFiles = [indexFile, modelFile];
    const targets = new Map(model.configurations[0].targets.map(item => {
        modelFiles.push(path.join(reply, item.jsonFile));
        const target = JSON.parse(fs.readFileSync(path.join(reply, item.jsonFile)));
        return [target.id, target];
    }));
    const roots = [...targets.values()].filter(target => target.name === 'cangjie-runtime');
    if (roots.length !== 1 || roots[0].type !== 'SHARED_LIBRARY') throw Error('runtime DLL target is missing or ambiguous');
    const root = roots[0];
    // CMake's actual link-library fragments distinguish linked archives from
    // order-only dependencies. Never infer this set from object-name prefixes.
    const libraries = new Set();
    for (const fragment of root.link?.commandFragments || []) {
        if (fragment.role !== 'libraries') continue;
        for (const token of fragment.fragment.matchAll(/"([^"]+)"|'([^']+)'|(\S+)/g))
            libraries.add(key(path.resolve(build, token[1] || token[2] || token[3])));
    }
    const selected = [root], visited = new Set([root.id]);
    function visit(target) {
        for (const dep of target.dependencies || []) {
            if (visited.has(dep.id)) continue;
            visited.add(dep.id);
            const next = targets.get(dep.id);
            if (!next) throw Error(`missing target dependency: ${dep.id}`);
            if (next.type === 'OBJECT_LIBRARY') throw Error(`unsupported embedded object-library link input: ${next.name}`);
            if (next.type !== 'STATIC_LIBRARY') continue;
            if (next.artifacts?.some(a => libraries.has(key(path.resolve(build, a.path))))) {
                selected.push(next);
                visit(next);
            } else {
                const trace = target.backtraceGraph;
                const command = trace?.commands?.[trace?.nodes?.[dep.backtrace]?.command];
                if (command === 'target_link_libraries') throw Error(`linked archive absent from link fragments: ${next.name}`);
            }
        }
    }
    visit(root);
    const commandsFile = path.join(build, 'compile_commands.json');
    const commands = JSON.parse(fs.readFileSync(commandsFile));
    const objects = [], closure = [];
    for (const target of selected) {
        const sources = [], outsideRuntime = [];
        const objectRoot = path.resolve(build, target.paths.build, 'CMakeFiles', target.name + '.dir');
        for (const src of target.sources || []) {
            if (src.compileGroupIndex === undefined) continue;
            const file = path.resolve(model.paths.source, src.path);
            if (!within(source, file)) { outsideRuntime.push(file); continue; }
            const matches = commands.filter(command => key(path.resolve(command.directory, command.file)) === key(file))
                .map(command => outputObject(command)).filter(object => object && within(objectRoot, object));
            if (matches.length !== 1) throw Error(`selected runtime source must have one object: ${target.name}:${src.path} (${matches.length})`);
            const relative = portable(path.relative(source, file));
            sources.push(relative);
            objects.push({target: target.id, source: relative, file, object: matches[0]});
        }
        closure.push({id: target.id, name: target.name, type: target.type,
            artifacts: target.artifacts || [], sources, outside_runtime: outsideRuntime});
    }
    if (!objects.length) throw Error('normal link graph contains no runtime source objects');
    return {objects, linkage: {root: root.id, targets: closure,
        index_sha256: sha(indexFile), codemodel_sha256: sha(modelFile)}, commandsFile, modelFiles};
}

export function symbolBlocks(text) {
    const blocks = []; let lines = null, depth = 0;
    for (const line of text.split('\n')) {
        if (lines === null) {
            if (!/^\s*Symbol\s*\{\s*$/.test(line)) continue;
            lines = []; depth = 1; continue;
        }
        if (/\{\s*$/.test(line)) depth++;
        if (/^\s*}\s*$/.test(line)) depth--;
        if (depth === 0) { blocks.push(lines.join('\n')); lines = null; } else lines.push(line);
    }
    if (lines !== null) throw Error('truncated COFF symbol table');
    return blocks;
}

function main() {
    const [buildArg, sourceArg, revision, outputArg, readobj = 'llvm-readobj'] = process.argv.slice(2);
    if (process.platform !== 'win32') throw Error('capture requires the actual native Windows build');
    if (!outputArg || !/^[a-f0-9]{40}$/.test(revision))
        throw Error('usage: capture_windows_runtime_definitions.mjs BUILD RUNTIME_SRC REVISION OUTPUT [LLVM_READOBJ]');
    const build = path.resolve(buildArg), source = path.resolve(sourceArg), output = path.resolve(outputArg);
    const plan = planRuntimeObjects(build, source), evidence = output + '.commands';
    fs.mkdirSync(evidence, {recursive: true});
    fs.writeFileSync(path.join(evidence, 'link-object-plan.json'), JSON.stringify(plan, null, 2));
    fs.mkdirSync(path.join(evidence, 'file-api'), {recursive: true});
    for (const input of plan.modelFiles) fs.copyFileSync(input, path.join(evidence, 'file-api', path.basename(input)));
    fs.copyFileSync(plan.commandsFile, path.join(evidence, 'compile_commands.json'));
    function read(label, args) {
        const p = spawnSync(readobj, args, {encoding: 'utf8', maxBuffer: 64 * 1024 * 1024});
        fs.writeFileSync(path.join(evidence, label + '.log'), (p.stdout || '') + (p.stderr || ''));
        fs.writeFileSync(path.join(evidence, label + '.json'), JSON.stringify({cmd: readobj, args,
            status: p.status, signal: p.signal, error: p.error?.message}, null, 2));
        if (p.status !== 0) throw Error(`${label}: readobj rc=${p.status}`);
        return p.stdout;
    }
    const version = read('version', ['--version']), objects = [];
    for (const object of plan.objects) {
        const text = read('object-' + objects.length, ['--file-headers', '--symbols', object.object]);
        if (!/Format:\s+COFF-/i.test(text)) throw Error(`not a COFF object: ${object.object}`);
        const defined = [];
        for (const block of symbolBlocks(text)) {
            if (!/StorageClass:\s+External\b/.test(block) || /Section:\s+IMAGE_SYM_UNDEFINED\b/.test(block)) continue;
            const name = block.match(/^\s*Name:\s*(\S+)\s*$/m)?.[1];
            if (name) defined.push(name);
        }
        objects.push({target: object.target, source: object.source, source_sha256: sha(object.file),
            object: portable(path.relative(build, object.object)), object_sha256: sha(object.object),
            defined: [...new Set(defined)].sort()});
    }
    const document = {system: 'Windows', revision, linkage: plan.linkage,
        compile_commands_sha256: sha(plan.commandsFile), readobj, version, objects};
    fs.writeFileSync(output + '.tmp', JSON.stringify(document, null, 2) + '\n');
    fs.renameSync(output + '.tmp', output);
    console.log(`WINDOWS_RUNTIME_DEFINITIONS objects=${objects.length} targets=${plan.linkage.targets.length} output=${output}`);
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
