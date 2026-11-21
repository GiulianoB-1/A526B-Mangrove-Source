#!/usr/bin/env python3
"""One-time original P479 ELF versus 55babd9 and NO-A3C provenance, NO FLASH."""
import collections,difflib,hashlib,json,os,re,shutil,subprocess,zipfile
from pathlib import Path
R=Path.cwd(); O=R/"p479-reference-audit"; O.mkdir(exist_ok=True)
T=Path(os.environ["RUNNER_TEMP"])/"p479-header-study";T.mkdir(exist_ok=True)
A=T/"reference"; A.mkdir(exist_ok=True)
def run(args,cwd=None,log=None):
    if log:
        with (O/log).open("w") as f:
            subprocess.run(args,cwd=cwd or R,stdout=f,stderr=subprocess.STDOUT,check=True)
    else: subprocess.run(args,cwd=cwd or R,check=True)
def sha(path):
    h=hashlib.sha256()
    with open(path,"rb") as f:
        for block in iter(lambda:f.read(1048576),b""):h.update(block)
    return h.hexdigest()
def save(name,data):(O/name).write_text(json.dumps(data,indent=2,sort_keys=True)+"\n")
assert os.environ.get("GH_TOKEN")
assert subprocess.check_output(["git","rev-list","-n1","p479-baseline"],text=True).strip()=="a040e6cf1e7232fc3e91516416fb7fffa3fc9025"
run(["git","merge-base","--is-ancestor","55babd9c651375580b3c5c10d7f37ac6213e005c","HEAD"])
# Source ZIP contains real P479 ELF labeled P471-D1, confirmed by exact Image+System.map.
run(["gh","release","download","p479-equivalence-reference","--repo","GiulianoB-1/A526B-Mangrove-Kernel","--pattern","p479-reference-14131467025.zip","--dir",str(A)])
archive=A/"p479-reference-14131467025.zip"
assert sha(archive)=="44412b170b639ebbbf6615e4a302ec8e4a83e09a934e32da49bbc1c801755230"
with zipfile.ZipFile(archive) as z:
    for member,name in [
        ("phase447b-variants/P471-D1-DATARMNET-CANDIDATE.vmlinux","vmlinux"),
        ("phase447b-variants/P471-D1-DATARMNET-CANDIDATE.System.map","System.map")
    ]:
        with z.open(member) as src, (A/name).open("wb") as dst:shutil.copyfileobj(src,dst)
    cmds=[n for n in z.namelist() if n.endswith(".o.cmd")]
assert sha(A/"System.map")=="26ae9bda166d764bf695d6c51b8799632bc6f1ea276747a9f8fd1d0caf6d3a3e"
run(["llvm-objcopy","-O","binary",str(A/"vmlinux"),str(A/"Image")])
assert sha(A/"Image")=="c1e29a64f611f402cdc8e5db1f42fea5b6ec9f48b19df60d5b9aa6da2d8585ae"
# Some dwarves versions emit a valid DWARF layout but exit 1 when .BTF is absent.
def layout(elf, log):
    try:
        run(["pahole","-C","subsys_desc",str(elf)],log=log)
    except subprocess.CalledProcessError:
        if "struct subsys_desc {" not in (O/log).read_text():
            raise

layout(A/"vmlinux","original-subsys_desc.txt")
original_layout=(O/"original-subsys_desc.txt").read_text()
assert "size: 464" in original_layout and "stop_ack_irq" in original_layout
save("P479-original-provenance.json",{"original_artifact":14131467025,"ELF_member_label":"P471-D1-DATARMNET-CANDIDATE.vmlinux","raw_Image_sha256":sha(A/"Image"),"System_map_sha256":sha(A/"System.map"),"original_cmd_records":cmds,"old_header_choice_from_cmd":"UNKNOWN" if not cmds else "SEE_DEPENDENCIES","flash_authorized":False})
print("P479 original ELF matches both authoritative hashes",flush=True)
# Clean direct-source builds, retaining all .o.cmd compiler dependency files.
src=T/"control-src"
run(["git","worktree","add","--detach",str(src),"55babd9c651375580b3c5c10d7f37ac6213e005c"])
def build(root,dest,conf):
    dest.mkdir(exist_ok=True);(dest/".config").write_text(conf)
    args=["make","-C",str(root),"O="+str(dest),"ARCH=arm64","CROSS_COMPILE=aarch64-linux-gnu-","CLANG_TRIPLE=aarch64-linux-gnu-","LLVM=1","LLVM_IAS=1"]
    run(args+["olddefconfig"],log=dest.name+"-olddefconfig.log")
    assert (dest/".config").read_text()==conf,"config drift"
    print("Compiling "+dest.name,flush=True)
    run(args+["-j"+str(os.cpu_count() or 2),"Image","vmlinux"],log=dest.name+"-compile.log")
    assert (dest/"vmlinux").is_file() and (dest/"System.map").is_file()
    layout(dest/"vmlinux",dest.name+"-subsys_desc.txt")
control=R/"out_CONTROL";no=R/"out_NO_A3C"
build(src,control,(src/"ci/build.config").read_text())
c=(R/"ci/build.config").read_text()
assert c.count("CONFIG_P480_A3C=y\n")==1
build(R,no,c.replace("CONFIG_P480_A3C=y\n","# CONFIG_P480_A3C is not set\n"))
# Real bloat-o-meter vs the original P479 linked ELF.
for name,before,after in [
 ("B-original-vs-control",A/"vmlinux",control/"vmlinux"),
 ("B-original-vs-NO-A3C",A/"vmlinux",no/"vmlinux"),
 ("C-control-vs-NO-A3C",control/"vmlinux",no/"vmlinux")]:
    run(["python3","scripts/bloat-o-meter","-c",str(before),str(after)],log=name+".bloat.txt")
def bloat(file):
    vals=[];section=None
    for line in file.read_text().splitlines():
        m=re.match(r"^(Function|Data|RO Data)\s+old\s+new\s+delta",line)
        if m:section=m[1]
        row=line.split()
        if section and len(row)==4 and re.fullmatch(r"[+-]\d+",row[3]):
            vals.append({"section":section,"symbol":row[0],"old":row[1],"new":row[2],"delta":int(row[3])})
    return {"count":len(vals),"all":vals,"top40":sorted(vals,key=lambda x:abs(x["delta"]),reverse=True)[:40]}
bresults={name:bloat(O/(name+".bloat.txt")) for name in ("B-original-vs-control","B-original-vs-NO-A3C","C-control-vs-NO-A3C")}
# Full name/order diff, separate from address differences.
def compare(old,new,label):
    a=[x.split(maxsplit=2) for x in old.read_text().splitlines()]
    b=[x.split(maxsplit=2) for x in new.read_text().splitlines()]
    al=[" ".join(x[1:])+"\n" for x in a];bl=[" ".join(x[1:])+"\n" for x in b]
    with (O/(label+".name-order.diff")).open("w") as f:
        f.writelines(difflib.unified_diff(al,bl,fromfile=str(old),tofile=str(new),n=2))
    diff=[]
    for i in range(max(len(a),len(b))):
        x=a[i] if i<len(a) else None;y=b[i] if i<len(b) else None
        if x==y:continue
        diff.append({"line":i+1,"old":x,"new":y,"address_only":x is not None and y is not None and x[1:]==y[1:]})
    save(label+".all-changes.json",diff)
    return {"equal":sha(old)==sha(new),"old_sha256":sha(old),"new_sha256":sha(new),"old_lines":len(a),"new_lines":len(b),"changed_lines":len(diff),"address_only":sum(x["address_only"] for x in diff),"name_or_order_changed":sum(not x["address_only"] for x in diff),"first20":diff[:20]}
maps={"original_control":compare(A/"System.map",control/"System.map","original-control"),"original_NO_A3C":compare(A/"System.map",no/"System.map","original-NO-A3C"),"control_NO_A3C":compare(control/"System.map",no/"System.map","control-NO-A3C")}
# All differing double-headers, including both include/uapi and compat/include/uapi.
dup={}
for orig in (R/"include").rglob("*.h"):
    if not orig.is_file():continue
    rel=orig.relative_to(R/"include").as_posix()
    twin=R/"a52-compat/include"/rel
    if twin.is_file() and sha(orig)!=sha(twin):
        dup[rel]={"include_sha256":sha(orig),"compat_sha256":sha(twin),"include":"include/"+rel,"compat":"a52-compat/include/"+rel}
def deps(folder):
    all_cmd=list(folder.rglob(".*.o.cmd"));found=[]
    for cmd in all_cmd:
        txt=cmd.read_text(errors="replace")
        if "deps_" not in txt:continue
        txt=txt.split("deps_",1)[1].split("\n\n",1)[0]
        for path in dup:
            inc=bool(re.search(r"(?<!a52-compat/)include/"+re.escape(path)+r"(?=[\s\\]|$)",txt))
            com=bool(re.search(r"a52-compat/include/"+re.escape(path)+r"(?=[\s\\]|$)",txt))
            if inc or com:found.append({"cmd":cmd.relative_to(folder).as_posix(),"header":path,"include_selected":inc,"compat_selected":com})
    return {"cmd_files":len(all_cmd),"affected_objects":len({x["cmd"] for x in found}),"dependencies":found}
control_dep=deps(control);no_dep=deps(no)
save("DUPLICATE-HEADER-OBJECT-DEPENDENCIES.json",{"number_different_duplicate_headers":len(dup),"headers":dup,"repaired":control_dep,"P480_NO_A3C":no_dep,"original_o_cmd_present":bool(cmds),"original_header_used":"UNKNOWN_WITHOUT_ORIGINAL_O_CMD","flash_authorized":False})
# Store full config diffs, never silently drop generated Kconfig changes.
cfg={}
for label,old,new in [("P479-original-to-control",R/"ci/P479.config",control/".config"),("control-to-NO-A3C",control/".config",no/".config")]:
    with (O/(label+".config.diff")).open("w") as f:f.writelines(difflib.unified_diff(old.read_text().splitlines(True),new.read_text().splitlines(True),fromfile=str(old),tofile=str(new)))
    cfg[label]={"old":sha(old),"new":sha(new),"same":sha(old)==sha(new)}
result={"reference_Image_matches_original":True,"reference_System_map_matches_original":True,"maps":maps,"bloat":bresults,"duplicate_headers":len(dup),"duplicate_header_objects":{"control":control_dep["affected_objects"],"NO_A3C":no_dep["affected_objects"]},"config":cfg,"flash_authorized":False}
save("P479-ORIGINAL-ELF-BLOAT-REVIEW.json",result)
with (O/"README.md").open("w") as f:
    f.write("# Original P479 binary, 55babd9 and NO-A3C review\n\n")
    f.write("Original P479 ELF was verified using the SHA-pinned raw Image and System.map; file is mislabeled P471-D1 in original artifact.\n\n")
    f.write(f"Differing duplicate headers: {len(dup)}. Dependent objects: {control_dep['affected_objects']} repaired, {no_dep['affected_objects']} NO-A3C.\n\n")
    for k,v in maps.items():f.write(f"{k}: map exact={v['equal']}; address-only lines={v['address_only']}; name/order lines={v['name_or_order_changed']}.\n\n")
    for k,v in bresults.items():f.write(f"{k}: {v['count']} changed-size symbol entries; see full .bloat.txt.\n\n")
    f.write("Historical original .o.cmd files absent; do not infer the old compiler include path. NO FLASH.\n")
with (O/"SHA256SUMS").open("w") as f:
    for x in sorted(O.iterdir()):
        if x.is_file() and x.name!="SHA256SUMS":f.write(sha(x)+"  "+x.name+"\n")
print((O/"README.md").read_text(),flush=True)
