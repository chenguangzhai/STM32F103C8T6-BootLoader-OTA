"""临时编译验证脚本：按工程 compile_commands.json 的参数编译全部 .c 并链接。
用法: python Tools/verify_build.py <工程目录名 APP|Recieve> <新源文件相对路径...>
编译数据库是旧的，新目录的 include 由本脚本按模板补齐。
"""
import json, subprocess, shlex, sys, os, re

proj = sys.argv[1]
extra_srcs = sys.argv[2:]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cwd = os.path.join(root, proj)
os.chdir(cwd)

cmds = json.load(open('build/Debug/compile_commands.json', encoding='utf-8'))
tpl = next(e for e in cmds if e['file'].lower().endswith('w25q.c'))

# 新目录的 include：两个工程都要 CRC；APP 还有 OTA；Recieve 的 OTA 已存在但 Flash 也在
INCS = ['-IApplication/BSP/CRC', '-IApplication/BSP/OTA', '-IApplication/BSP/Flash']
def addinc(c):
    out = c
    for inc in INCS:
        if inc not in out:
            out = out.replace('-IApplication/BSP/Board', '-IApplication/BSP/Board ' + inc)
    return out

all_cmds = []
for e in cmds:
    e2 = dict(e)
    e2['command'] = addinc(e['command'])
    all_cmds.append(e2)

existing = {os.path.basename(e['file']).lower() for e in cmds}
for src in extra_srcs:
    if os.path.basename(src).lower() in existing:
        print(f'[{proj}] skip {src} (already in compile db)')
        continue
    obj = './build/Debug/.obj/' + src.replace('.c', '.o')
    cmd = re.sub(r'-o \S+ -MMD \./\S+', '-o ' + obj + ' -MMD ./' + src, addinc(tpl['command']))
    all_cmds.append({'directory': tpl['directory'], 'file': src, 'command': cmd})
    os.makedirs(os.path.dirname(obj), exist_ok=True)

fail = []
for e in all_cmds:
    r = subprocess.run(shlex.split(e['command']), cwd=e['directory'], capture_output=True, text=True)
    if r.returncode != 0:
        fail.append((e['file'], r.stderr))
print(f'[{proj}] COMPILE {len(all_cmds)} files, FAIL {len(fail)}')
for f, err in fail:
    print('---', f, '---\n', err[:2500])
if fail:
    sys.exit(1)

objs = [re.search(r'-o\s+(\S+\.o)', e['command']).group(1)
        for e in all_cmds if re.search(r'-o\s+(\S+\.o)', e['command'])]
gcc = r'c:\Users\qinghan\.eide\tools\gcc_arm\bin\arm-none-eabi-gcc.exe'
out = 'build/Debug/verify.elf'
link = [gcc, '-mcpu=cortex-m3', '-mthumb', '-Og', '-g', '-ffunction-sections',
        '-fdata-sections', '-funsigned-char', '--specs=nano.specs', '--specs=nosys.specs',
        '-T', 'STM32F103XX_FLASH.ld', '-Wl,--gc-sections',
        '-Wl,-Map=build/Debug/verify.map', '-lm', '-o', out] + objs
r = subprocess.run(link, capture_output=True, text=True)
print(f'[{proj}] LINK rc={r.returncode}')
if r.returncode:
    print(r.stderr[-2500:])
    sys.exit(1)
