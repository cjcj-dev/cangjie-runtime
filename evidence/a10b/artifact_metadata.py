from pathlib import Path
import hashlib,json,os,subprocess,re
root=Path('/root/sym_cangjie_runtime_494_implement_r5655389950')
def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
result={'root':'kkk2:'+str(root),'jobs':int(subprocess.check_output(['nproc'],text=True)),
        'parallel_arms':2,'cpus':sorted(os.sched_getaffinity(0)),
        'uptime_before':(root/'uptime-before.txt').read_text(),
        'uptime_after':(root/'uptime-after.txt').read_text(),
        'source_archive_sha256':digest(root/'source.tar.gz'),'arms':{}}
for arm in ['default','testable']:
 cache=(root/arm/'build/CMakeCache.txt').read_text()
 result['arms'][arm]={'configure_rc':(root/(arm+'-configure.rc')).read_text().strip(),
                      'build_rc':(root/(arm+'-build.rc')).read_text().strip(),
                      'wall_seconds':(root/(arm+'-wall.txt')).read_text().strip(),
                      'commit':re.search(r'^CJ_RUNTIME_COMMIT[^=]*=(.*)$',cache,re.M).group(1),
                      'so':{str(p):digest(p) for p in (root/arm/'build').rglob('*.so') if p.is_file()}}
text=json.dumps(result,indent=2)+'\n'
(root/'artifact-metadata.json').write_text(text)
print(text,end='')
# Source bundle is no longer needed after identity capture. Build outputs/logs remain.
(root/'source.tar.gz').unlink()
