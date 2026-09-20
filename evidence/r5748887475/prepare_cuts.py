import pathlib, subprocess, sys
repo = pathlib.Path(sys.argv[1])
head = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
root = pathlib.Path('/root/cj_build/agent_scratch/sym_cangjie_runtime_581_implement_r5748887475')
mutations = {
 'head_cut': ('runtime/src/Mutator/Mutator.h', '            MutatorUnlock();\n            break;', '            MutatorUnlock();\n            return;'),
 'jni_enter_cut': ('runtime/src/CompilerCalls.cpp', '    ZJNICritical::enter();', '    // control: enter disconnected'),
 'jni_block_cut': ('runtime/src/Heap/z/zGeneration.cpp', '        if (block_jni_critical()) {\n            ZJNICritical::block();\n        }', '        if (block_jni_critical()) {\n            // control: block disconnected\n        }'),
 'vector_cut': ('runtime/src/Mutator/Mutator.h', '    std::deque<ObjectRef> nativeFrameRoots;', '    std::vector<ObjectRef> nativeFrameRoots;'),
 'argument_producer_cut': ('runtime/src/ObjectModel/MethodInfo.cpp', '                argValues->AddHandle(structHandle, TYPEINFO_PTR_SIZE);', '                argValues->AddInt64(reinterpret_cast<Uptr>(dst));'),
 'argument_consumer_cut': ('runtime/src/ObjectModel/ArgValue.h', '        for (const auto& argument : handles) {', '        for (const auto& argument : std::vector<HandleArgument>{}) {'),
}
root.mkdir(parents=True, exist_ok=True)
for arm in sys.argv[2:]:
    tree = root / arm
    subprocess.run(['git', '-C', str(repo), 'worktree', 'add', '--detach', str(tree), head], check=True)
    if arm in mutations:
        file, old, new = mutations[arm]
        path = tree / file
        source = path.read_text()
        assert source.count(old) == 1, (arm, source.count(old), old)
        path.write_text(source.replace(old, new))
        patch = subprocess.check_output(['git', '-C', str(tree), 'diff'], text=True)
        (repo/'evidence/r5748887475'/f'{arm}.diff').write_text(patch)
    print(arm, head, tree)
