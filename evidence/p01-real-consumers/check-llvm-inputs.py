import pathlib,hashlib,json,sys
source=pathlib.Path(sys.argv[1]); manifest=json.loads(pathlib.Path(sys.argv[2]).read_text())
rows=[]
for expected in manifest['files']:
 p=source/expected['path']; actual=hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None
 rows.append(dict(path=expected['path'],expected=expected['sha256'],actual=actual,equal=actual==expected['sha256']))
print(json.dumps(rows,indent=2))
sys.exit(0 if all(r['equal'] for r in rows) else 1)
