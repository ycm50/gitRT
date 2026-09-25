# 把 .github/workflows/build.yml 里每个 step 的 run 体抽成独立 .ps1（按 Release 分支替换表达式），
# 便于在本机逐条跑，等价于"本地复现 CI 步骤"。
import yaml, os, re, json, shutil

with open('.github/workflows/build.yml', encoding='utf-8') as f:
    d = yaml.safe_load(f)

# ★ 先清空：否则旧版本抽出的步骤文件会残留、被一起跑（踩过）
if os.path.isdir('build/ci-steps'):
    shutil.rmtree('build/ci-steps', ignore_errors=True)
out = 'build/ci-steps'
os.makedirs(out, exist_ok=True)
manifest = []
for job in d['jobs']:
    for i, s in enumerate(d['jobs'][job]['steps']):
        if 'run' not in s:
            continue
        name = s.get('name', 'step%d' % i)
        body = s['run']
        body = body.replace("${{ matrix.config == 'Debug' && 'debug' || 'release' }}", "release")
        body = body.replace("${{ matrix.config }}", "Release")
        body = body.replace("${{ github.ref_name }}", "v0.1.0.0")
        body = body.replace("${{ steps.msys2.outputs.msys2-location }}", "A:/msys64")
        slug = re.sub(r'[^A-Za-z0-9]+', '-', name).strip('-') or ('step%d' % i)
        fn = '%s/%s-%02d-%s.ps1' % (out, job, i, slug[:40])
        with open(fn, 'w', encoding='utf-8') as g:
            g.write(body + '\n')
        manifest.append({'job': job, 'i': i, 'name': name, 'file': fn,
                         'continue_on_error': bool(s.get('continue-on-error'))})
with open(out + '/manifest.json', 'w', encoding='utf-8') as f:
    json.dump(manifest, f, ensure_ascii=False, indent=1)
print('抽取步骤数:', len(manifest))
for m in manifest:
    print('  %-8s %2d  %s -> %s' % (m['job'], m['i'], m['name'], m['file']))
