using System.Reflection;
using System.Runtime.Loader;
using System.Numerics;
using System.Security.Cryptography;
using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Net;
using System.Net.Sockets;
using System.Diagnostics;

var assembly=AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(args[0]));
var protocol=assembly.GetType("GatewayCapture.Realtime.TgcpProtocol",true)!;
var udp=assembly.GetType("GatewayCapture.Realtime.UdpDecoder",true)!;
object Call(Type t,string method,params object[] p)=>t.GetMethod(method,BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static)!.Invoke(null,p)!;
byte[] Data(int n,int seed)=>Enumerable.Range(0,n).Select(i=>(byte)(i*37+seed*19)).ToArray();
string Hex(byte[] b)=>Convert.ToHexString(b).ToLowerInvariant();
byte[] EncryptUdp(byte[] input,byte[] bank){var b=input.ToArray();for(int off=0;off+8<=b.Length;off+=8){uint a=BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(off)),c=BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(off+4)),sum=0;int k=(off/8%8)*16;uint Key(uint i)=>BinaryPrimitives.ReadUInt32LittleEndian(bank.AsSpan(k+(int)i*4));unchecked{for(int round=0;round<32;round++){a+=(((c<<4)^(c>>5))+c)^(sum+Key(sum&3));sum+=0x9e3779b9u;c+=(((a<<4)^(a>>5))+a)^(sum+Key((sum>>11)&3));}}BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(off),a);BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(off+4),c);}return b;}
var aes=new List<object>();for(int n=0;n<96;n++){var p=Data(n,3);var k=Data(16,7);aes.Add(new{plain=Hex(p),key=Hex(k),cipher=Hex((byte[])Call(protocol,"Encrypt",p,k))});}
var packets=new List<object>();for(int seed=1;seed<=3;seed++)for(int n=0;n<160;n++){var p=Data(n,seed);var k=Data(128,seed+6);var c=EncryptUdp(p,k);var actual=(byte[])Call(udp,"Decrypt",c,k);if(!actual.SequenceEqual(p))throw new Exception("oracle UDP mismatch");packets.Add(new{plain=Hex(actual),bank=Hex(k),cipher=Hex(c)});}
var envelopes=new List<object>();void Envelope(byte[] b){envelopes.Add(new{payload=Hex(b),valid=(bool)Call(udp,"ValidEnvelope",b)});}
// Valid empty-bunch envelopes, optional header flags, and reliable/partial/name branches.
for(int variant=0;variant<40;variant++){
 var bits=new List<int>{0};void Put(uint value,int count){for(int j=0;j<count;j++)bits.Add((int)((value>>j)&1));}
 Put(0,4);while(bits.Count<65)bits.Add(0);Put((uint)(variant%2),1);if(variant%2!=0)Put(7,8);Put(9,8);
 if(variant>=2){bool reliable=variant%3==0,open=variant%5==0,partial=variant%7==0;Put(open?1u:0u,1);if(open){Put(1,1);Put(0,1);}Put(0,1);Put(reliable?1u:0u,1);Put(0,1);Put(4,8);Put(0,2);Put(partial?1u:0u,1);if(reliable)Put(1,10);if(partial)Put(0,2);if(reliable||open){bool hardcoded=variant%2==0;Put(hardcoded?1u:0u,1);if(hardcoded)Put(2,8);else{Put(2,32);Put(65,8);Put(0,8);Put(0,32);}}Put(8,13);Put(43,8);}
 Put(1,1);Put(1,1);var b=new byte[(bits.Count+7)/8];for(int i=0;i<bits.Count;i++)b[i/8]|=(byte)(bits[i]<<(i%8));Envelope(b);for(int i=0;i<b.Length;i++){var corrupt=b.ToArray();corrupt[i]^=255;Envelope(corrupt);}for(int i=0;i<b.Length;i++)Envelope(b[..i]);
}
var rng=new Random(90401);for(int n=0;n<4000;n++){var b=new byte[rng.Next(0,260)];rng.NextBytes(b);Envelope(b);}
var prime=(BigInteger)protocol.GetField("Prime")!.GetValue(null)!;
var dh=new List<object>();for(int i=1;i<=80;i++){var secret=new BigInteger(i);var peer=BigInteger.ModPow(2,i+123,prime);var pub=BigInteger.ModPow(2,secret,prime).ToByteArray(true,true);var padded=new byte[64];pub.CopyTo(padded,64-pub.Length);dh.Add(new{secret=Hex(secret.ToByteArray(true,true)),@public=Hex(padded),peer=Hex(peer.ToByteArray(true,true)),key=Hex((byte[])Call(protocol,"Derive",peer,secret))});}
// A shared value of 2 must be hashed as one byte, not 64 padded bytes.
dh.Add(new{secret="01",@public=new string('0',126)+"02",peer="02",key=Hex((byte[])Call(protocol,"Derive",new BigInteger(2),BigInteger.One))});
File.WriteAllText(args[1],JsonSerializer.Serialize(new{assemblySha256=Hex(SHA256.HashData(File.ReadAllBytes(args[0]))),synthetic=true,aes,udp=packets,envelopes,dh}));
Console.WriteLine($"Wrote synthetic oracle: {aes.Count} AES, {packets.Count} UDP, {envelopes.Count} envelope, {dh.Count} DH cases.");
if(args.Length<4)return;
// Full client <-> C++ relay <-> mock server test on loopback, no real service.
byte[] Pad(byte[] plain){int n=(plain.Length%16<11?16:32)-plain.Length%16;var b=new byte[plain.Length+n];plain.CopyTo(b,0);Encoding.ASCII.GetBytes("tsf4g").CopyTo(b,b.Length-6);b[^1]=(byte)n;return b;}
var iv=Enumerable.Range(0,16).Select(i=>(byte)i).ToArray();
byte[] Encrypt(byte[] plain,byte[] key){using var cipher=Aes.Create();cipher.Key=key;return cipher.EncryptCbc(Pad(plain),iv,PaddingMode.None);}
byte[] Decrypt(byte[] bytes,byte[] key){using var cipher=Aes.Create();cipher.Key=key;var b=cipher.DecryptCbc(bytes,iv,PaddingMode.None);return b[..^b[^1]];}
byte[] Derive(BigInteger peer,BigInteger secret)=>MD5.HashData(BigInteger.ModPow(peer,secret,prime).ToByteArray(true,true));
byte[] Wire(bool up,int command,byte[] body,BigInteger? pub=null){int h=pub.HasValue?88:21;var b=new byte[h+body.Length];Convert.FromHexString("3366000B000C").CopyTo(b,0);BinaryPrimitives.WriteUInt16BigEndian(b.AsSpan(6),(ushort)command);b[8]=(byte)(up?0:1);BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(9),1);BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(13),(uint)h);BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(17),(uint)body.Length);if(pub.HasValue){b[21]=3;b[23]=64;var p=pub.Value.ToByteArray(true,true);p.CopyTo(b,88-p.Length);}body.CopyTo(b,h);return b;}
async Task<byte[]> ReadFrame(NetworkStream stream){var header=new byte[21];await stream.ReadExactlyAsync(header).AsTask().WaitAsync(TimeSpan.FromSeconds(6));int h=(int)BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(13)),b=(int)BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(17));var result=new byte[h+b];header.CopyTo(result,0);await stream.ReadExactlyAsync(result.AsMemory(21)).AsTask().WaitAsync(TimeSpan.FromSeconds(6));return result;}
using var listener=new TcpListener(IPAddress.Loopback,0);listener.Start();int upstream=((IPEndPoint)listener.LocalEndpoint).Port;
using var reserve=new TcpListener(IPAddress.Loopback,0);reserve.Start();int relay=((IPEndPoint)reserve.LocalEndpoint).Port;reserve.Stop();
var start=new ProcessStartInfo(Path.GetFullPath(args[2])){UseShellExecute=false,CreateNoWindow=true,RedirectStandardError=true,RedirectStandardOutput=true};foreach(var arg in new[]{"capture","--relay","true","--listen",$"127.0.0.1:{relay}","--upstream",$"127.0.0.1:{upstream}","--device","synthetic-loopback","--synthetic","true","--seconds","5","--out",Path.GetFullPath(args[3])})start.ArgumentList.Add(arg);
using var proc=Process.Start(start)!;var stderr=proc.StandardError.ReadToEndAsync();
try{
 using var client=new TcpClient();for(int attempt=0;;attempt++){try{await client.ConnectAsync(IPAddress.Loopback,relay);break;}catch(SocketException)when(attempt<20){await Task.Delay(100);}}
 using var server=await listener.AcceptTcpClientAsync().WaitAsync(TimeSpan.FromSeconds(5));var cs=client.GetStream();var ss=server.GetStream();BigInteger a=1234567,bsecret=7654321;var A=BigInteger.ModPow(2,a,prime);var B=BigInteger.ModPow(2,bsecret,prime);var hello=Wire(true,0x1001,[],A);await cs.WriteAsync(hello.AsMemory(0,15));await Task.Delay(30);await cs.WriteAsync(hello.AsMemory(15));var sent=await ReadFrame(ss);var Y=new BigInteger(sent.AsSpan(24,64),true,true);if(Y==A)throw new Exception("client DH not replaced");var serverKey=Derive(Y,bsecret);var bank=Data(128,19);var message=Encoding.ASCII.GetBytes("bank="+Convert.ToBase64String(bank)+";end");await ss.WriteAsync(Wire(false,0x1002,Encrypt(message,serverKey),B));var recv=await ReadFrame(cs);var X=new BigInteger(recv.AsSpan(24,64),true,true);if(X==B)throw new Exception("server DH not replaced");var clientKey=Derive(X,a);if(!Decrypt(recv[88..],clientKey).SequenceEqual(message))throw new Exception("server hello body translation");
 var req=Data(43,3);await cs.WriteAsync(Wire(true,0x2001,Encrypt(req,clientKey)));if(!Decrypt((await ReadFrame(ss))[21..],serverKey).SequenceEqual(req))throw new Exception("request translation");var reply=Data(29,7);await ss.WriteAsync(Wire(false,0x2002,Encrypt(reply,serverKey)));if(!Decrypt((await ReadFrame(cs))[21..],clientKey).SequenceEqual(reply))throw new Exception("reply translation");
 client.Client.Shutdown(SocketShutdown.Send);server.Client.Shutdown(SocketShutdown.Send);await proc.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));if(proc.ExitCode!=0)throw new Exception(await stderr);
 var records=File.ReadLines(args[3]).Select(line=>JsonDocument.Parse(line)).ToArray();if(!records.Any(e=>e.RootElement.GetProperty("type").GetString()=="bank_candidate"))throw new Exception("bank not observed");if(records.Any(e=>e.RootElement.GetProperty("type").GetString()=="tcp_error"))throw new Exception("unexpected relay error");Console.WriteLine("Loopback PASS: fragmented DH, both public substitutions, encrypted hello, bank extraction, bidirectional TCP translation.");
}finally{if(!proc.HasExited)proc.Kill(entireProcessTree:true);}
