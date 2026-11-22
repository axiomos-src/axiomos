import struct
img=open(r"D:\Axiom-OS\vm\axiom-flat.img","rb").read()
ESP=2048
bps=struct.unpack_from("<H",img,ESP*512+11)[0]
spc=img[ESP*512+13]
rsv=struct.unpack_from("<H",img,ESP*512+14)[0]
nfat=img[ESP*512+16]
fatsz=struct.unpack_from("<I",img,ESP*512+36)[0]
root=struct.unpack_from("<I",img,ESP*512+44)[0]
data_off=ESP+(rsv+nfat*fatsz)
fat_off=ESP+rsv
def clusters(c):
    out=[]
    while c and c<0x0FFFFFF8 and len(out)<10000:
        out.append(c)
        nxt=struct.unpack_from("<I",img,(fat_off+ (c//128))*512 + (c%128)*4)[0]
        c=nxt
    return out
def readdir(c):
    ents=[]
    for cl in clusters(c):
        base=(data_off+(cl-2)*spc)*512
        for i in range(spc*512//32):
            e=img[base+i*32:base+i*32+32]
            if e[0]==0: break
            if e[0]==0xE5: continue
            name=e[0:11].decode("latin1").rstrip()
            attr=e[11]; clu=struct.unpack_from("<H",e,26)[0] | (struct.unpack_from("<H",e,20)[0]<<16); size=struct.unpack_from("<I",e,28)[0]
            ents.append((name,attr,clu,size))
    return ents
print("EFI dir:",readdir(root))
efi=[e for e in readdir(root) if e[0]=="EFI"][0]
print("EFI:",readdir(efi[2]))
boot=[e for e in readdir(efi[2]) if e[0]=="BOOT"][0]
ents=readdir(boot[2])
print("BOOT:",ents)
f=[e for e in ents if e[0].startswith("BOOTX64")][0]
data=b"".join(img[(data_off+(c-2)*spc)*512:(data_off+(c-2)*spc)*512+spc*512] for c in clusters(f[2]))
data=data[:f[3]]
print("chain len",len(clusters(f[2])),"size",f[3],"head",data[:2])
ref=open(r"D:\Axiom-OS\build\esp\EFI\BOOT\BOOTX64.EFI","rb").read() if __import__("os").path.exists(r"D:\Axiom-OS\build\esp\EFI\BOOT\BOOTX64.EFI") else None
print("ref",None if ref is None else (len(ref), ref[:2], ref==data))
ax=[e for e in readdir(root) if e[0]=="AXIOMOS"][0]
print("AXIOMOS:",readdir(ax[2]))
