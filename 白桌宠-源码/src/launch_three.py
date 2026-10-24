# 一键起三只白：主管白(max) + 白2(flash) + 白3(plus)
# 同端点同 key（settings.json），换的只是模型；共享 memory\about_bro.txt，三只都认得哥哥
import subprocess, os, time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "pet.exe")
INST = [
    ["--slot", "0", "--name", "白",  "--model", "qwen3.8-max",   "--backend", "local"],
    ["--slot", "1", "--name", "白2", "--model", "qwen3.8-flash", "--backend", "local"],
    ["--slot", "2", "--name", "白3", "--model", "qwen3.6-plus",  "--backend", "local"],
]
subprocess.run(["taskkill", "/IM", "pet.exe", "/F"], capture_output=True)
time.sleep(1.2)
ps = [subprocess.Popen([EXE] + a, cwd=HERE, close_fds=True) for a in INST]
time.sleep(4)
print("alive %d/3" % sum(1 for p in ps if p.poll() is None))
