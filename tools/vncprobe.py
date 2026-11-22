import socket,struct,sys
s=socket.create_connection(("127.0.0.1",5900),timeout=10)
print("ver",s.recv(12))
s.sendall(b"RFB 003.008\n")
n=struct.unpack(">B",s.recv(1))[0]
types=s.recv(n)
print("nsec",n,"types",list(types))
s.close()
