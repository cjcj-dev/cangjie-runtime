from pathlib import Path
import re
root=Path('/root/sym_cangjie_runtime_608_implement_r5676392826/llvm/evidence')
s=(root/'p01-array-swap.mir').read_text()
# Keep the target machine function. Retain machine debug instruction numbers,
# which trigger this defect, while removing source-level metadata references.
body=s[s.index('\n---\nname:'):]
body=re.sub(r',? debug-location !\d+','',body)
body=re.sub(r'(\b(?:%?bb\.\d+))\.[A-Za-z0-9_$.-]+', r'\1', body)
body=re.sub(r' \(%ir-block\.[^)]+\)', '', body)
body=re.sub(r' :: [^\n]*', '', body)
body=re.sub(r'^    DBG_(?:VALUE|INSTR_REF|LABEL).*\n','',body,flags=re.M)
body=re.sub(r'^debugValueSubstitutions:.*?(?=^[A-Za-z])','debugValueSubstitutions: []\n',body,flags=re.M|re.S)
body=re.sub(r', debug-info-variable: !\d+','',body)
body=re.sub(r', debug-info-expression: !\d+','',body)
body=re.sub(r', debug-info-location: !\d+','',body)
# Declare globals referenced by machine operands, so no unrelated std metadata
# is needed to parse the machine function.
names=set(re.findall(r'@("[^"]+"|[-a-zA-Z$._0-9]+)',body))
fn='_CNat5ArrayIG_E4swapHll'
ir='target triple = "x86_64-unknown-linux-gnu"\n'
ir+='define void @'+fn+'() gc "cangjie" { ret void }\n'
for n in sorted(names):
 if n==fn:continue
 if n.startswith(('CJ_', '"_CN', '"rt$')) or n == 'SetDebugLocation':
  ir+='declare void @'+n+'(...)\n'
 else:
  ir+='@'+n+' = external global i8\n'
(root/'p01-spiller-small.mir').write_text('--- |\n'+''.join('  '+l+'\n' for l in ir.splitlines())+'...\n'+body.lstrip('\n'))
