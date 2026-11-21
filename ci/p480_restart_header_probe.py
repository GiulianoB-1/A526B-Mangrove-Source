#!/usr/bin/env python3
"""Read-only DWARF check for P480 restart-ABI objects. NO FLASH."""
import json, os, re, subprocess, sys
from pathlib import Path

root=Path.cwd()
out=root/"out_restart_dwarf"
evidence=root/"p480-abi-gate-evidence"
evidence.mkdir(exist_ok=True)
ordinary=[
 "drivers/a52_secure/qseecom.o",
 "drivers/a52_secure/qsee_ipc_irq_bridge.o",
 "drivers/gpu/msm/adreno_a5xx.o",
 "drivers/gpu/msm/adreno_a6xx.o",
 "drivers/a52_pil/peripheral-loader.o",
 "drivers/soc/qcom/p472_tg_audio/ipc/apr.o",
 "drivers/soc/qcom/p472_tg_audio/dsp/audio_ssr.o",
]
both=[
 "drivers/platform/msm/ipa/ipa_v3/ipa.o",
 "drivers/platform/msm/ipa/ipa_v3/rmnet_ipa.o",
 "drivers/platform/msm/ipa/ipa_v3/ipa_qmi_service.o",
 "drivers/a52_pil/subsystem_restart.o",
]
targets=ordinary+both
assert len(set(targets))==11

def cmd(args,log=None,required=True):
    result=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    if log:(evidence/log).write_text(result.stdout)
    if required and result.returncode:
        raise RuntimeError(" ".join(args)+" FAILED\n"+result.stdout[-2000:])
    return result

def layout(obj,name):
    r=cmd(["pahole","-I","-C",name,str(obj)],required=False)
    text=r.stdout
    if not re.search(r"\bstruct "+re.escape(name)+r"\s*\{",text):
        return {"status":"ABSENT","diagnostic":text[:500]}
    expected=({"size":464,"err_fatal_handler":336,"stop_ack_handler":344,
               "shutdown_ack_handler":352,"ramdump_disable_handler":360,
               "wdog_bite_handler":368,"generic_handler":376,
               "stop_ack_irq":400,"generic_irq":408}
              if name=="subsys_desc"
              else {"size":1424,"desc":0,"err_ready":1336,"shutdown_ack":1368})
    found={}
    size=re.search(r"/\*\s*size:\s*(\d+)",text)
    found["size"]=int(size.group(1)) if size else None
    for k in expected:
        if k=="size":continue
        m=re.search(r"(?:\(\*\s*"+re.escape(k)+r"\)\([^;]*;|\b"+re.escape(k)+r"\s*;)\s*/\*\s*(\d+)\s+",text)
        found[k]=int(m.group(1)) if m else None
    return {"status":"MATCH_ORIGINAL_P479" if found==expected else "ABI_MISMATCH",
            "expected":expected,"found":found,
            "first_lines":text.splitlines()[:12]}

config=(root/"ci/build.config").read_text()
assert config.count("CONFIG_P480_A3C=y\n")==1
config=config.replace("CONFIG_P480_A3C=y\n","# CONFIG_P480_A3C is not set\n")
out.mkdir(exist_ok=True)
(out/".config").write_text(config)
flags=["make","O="+str(out),"ARCH=arm64","CROSS_COMPILE=aarch64-linux-gnu-",
       "CLANG_TRIPLE=aarch64-linux-gnu-","LLVM=1","LLVM_IAS=1","LOCALVERSION="]
cmd(flags+["olddefconfig"],"olddefconfig.log")
assert (out/".config").read_text()==config
cmd(flags+["prepare","scripts"],"prepare.log")
compilation=cmd(flags+["-j8"]+ordinary+["drivers/platform/msm/ipa/ipam.o", "drivers/a52_pil/subsystem_restart.o"],"target-objects.log",required=False)
print("Target object make status:",compilation.returncode,flush=True)
rows=[]
failures=[]
for path in targets:
    obj=out/path
    row={"object":path,"both_header_copies_expected":path in both}
    if not obj.exists():
        row["status"]="NOT_COMPILED"
        failures.append(path+": NOT COMPILED")
    else:
        row["dwarf"]=(".debug_info" in cmd(["readelf","-SW",str(obj)]).stdout)
        if not row["dwarf"]:failures.append(path+": NO DWARF")
        row["subsys_desc"]=layout(obj,"subsys_desc")
        row["subsys_device"]=layout(obj,"subsys_device")
        for t in ("subsys_desc","subsys_device"):
            if row[t]["status"]=="ABI_MISMATCH":failures.append(path+": "+t+" mismatch")
        dep=obj.parent/("."+obj.name+".cmd")
        row["cmd_exists"]=dep.is_file()
        if dep.is_file():
            deps=dep.read_text(errors="replace")
            row["mentions_include_copy"]=bool(re.search(
                r"(?<!a52-compat/)include/soc/qcom/subsystem_restart.h(?=[\s\\]|$)",deps))
            row["mentions_compat_copy"]=bool(re.search(
                r"a52-compat/include/soc/qcom/subsystem_restart.h(?=[\s\\]|$)",deps))
    rows.append(row)
summary={"P479_subsys_desc_expected_size":464,
         "P479_subsys_device_expected_size":1424,
         "ordinary_only_objects":7,"both_copy_objects":4,
         "unique_objects":11,
         "note":"11 include-copy objects total; 4 of them also use compat, not 15 distinct objects",
         "target_make_status":compilation.returncode,
         "objects":rows,"failures":failures,"flash_authorized":False}
(evidence/"result.json").write_text(json.dumps(summary,indent=2)+"\n")
md=["# P480 restart-header DWARF gate","",
    "NO FLASH. 11 unique compiler objects: 7 ordinary-only, 4 both-copy.",
    "Original P479: subsys_desc size 464, subsys_device size 1424.",
    "","| Object | desc | device |","|---|---|---|"]
for x in rows:
    md.append("| "+x["object"]+" | "+x.get("subsys_desc",{}).get("status","NOT_COMPILED")
              +" | "+x.get("subsys_device",{}).get("status","NOT_COMPILED")+" |")
md+=["","Result: "+("PASS" if not failures else "BLOCKED"),""]
(evidence/"README.md").write_text("\n".join(md))
print("\n".join(md),flush=True)
if failures:
    print("BLOCKERS:",*failures,sep="\n",flush=True)
sys.exit(1 if failures else 0)
