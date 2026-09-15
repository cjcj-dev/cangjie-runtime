from pathlib import Path
import shutil,json
prefix='sym_cangjie_runtime_606_implement_r5674249495-'
keep=Path('/root')/(prefix+'final2')
rows=[]
for root in Path('/root').glob(prefix+'*'):
 if not root.is_dir():continue
 row={'root':str(root),'objects_removed':0,'archives_removed':[],'source_copies_removed':[]}
 archive=root/'source.tar.gz'
 if archive.is_file():archive.unlink();row['archives_removed'].append(str(archive))
 for cfg in ['default','testable']:
  build=root/cfg/'build'
  if build.is_dir():
   for obj in build.rglob('*.o'):
    obj.unlink();row['objects_removed']+=1
  source=root/cfg/'runtime'
  if root!=keep and source.is_dir():
   shutil.rmtree(source);row['source_copies_removed'].append(str(source))
 rows.append(row)
# Keep final runtime/test source for review replay. All final product SOs,
# test ELFs, identities and logs retain their original paths.
(keep/'cleanup.json').write_text(json.dumps(rows,indent=2))
print('cleaned own roots:',len(rows),'objects:',sum(x['objects_removed'] for x in rows))
