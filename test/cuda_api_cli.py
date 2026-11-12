import subprocess, sys
b=sys.argv[1]
def check(args,ok):
 p=subprocess.run([b,*args,'--json-schema'],capture_output=True,text=True)
 assert (p.returncode==0)==ok,(args,p.returncode,p.stderr)
for selector in ('cuda-api','cuda-api=all','cuda-api=nv-smi','cuda-api=0','cuda-api=-1','cuda-api=1x'):
 check(['-f',selector],False)
check(['-f','cuda-api=1','-f','cuda-api=1','-f','cuda-api=2'],True)
check(['--seal=uprobe-args','-U','uspan:sample(pid:1,arg:ret:s32/name(status))'],False)
check(['-U','uspan:sample(pid:1,arg:ret:s32/name(status))','--seal=uprobe-args'],True)
check(['-f','cuda=1','-f','cuda-api=1'],True)
check(['-f','cuda-api=1','--stacks=utrace'],True)
check(['-U','u:sample(pid:1)'],True)
print('CLI checks PASS')
