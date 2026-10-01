#include "coop.cpp"
#include <cassert>
#include <limits>
#include <filesystem>

std::vector<uint8_t> memory;
unsigned creates=0,destroys=0,poses=0,nudges=0,stream_refs=0;
void* Ptr(uint32_t a) { assert(a>=0x82000000 && a<0x84000000); return memory.data()+a-0x82000000; }
uint32_t Read32(uint32_t a) { uint32_t v; std::memcpy(&v,Ptr(a),4); return _byteswap_ulong(v); }
void Write32(uint32_t a,uint32_t v) { v=_byteswap_ulong(v); std::memcpy(Ptr(a),&v,4); }
void Write16(uint32_t a,uint16_t v) { v=_byteswap_ushort(v); std::memcpy(Ptr(a),&v,2); }
void Write8(uint32_t a,uint8_t v) { *static_cast<uint8_t*>(Ptr(a))=v; }
void WriteFloat(uint32_t a,float v) { uint32_t bits; std::memcpy(&bits,&v,4); Write32(a,bits); }
float ReadFloat(uint32_t a) { const uint32_t bits=Read32(a); float v; std::memcpy(&v,&bits,4); return v; }
int GuestCall(WmlContext* raw,uint32_t fn) {
  auto& c=*reinterpret_cast<PPCContext*>(raw);
  if(fn==0x824C2CF8) {
    ++nudges;
  } else if(fn==0x8250C750) {
    assert(c.r3.u32==0x83E876B0+5*204 && c.r4.u32==0x05000012);
    assert(c.r5.u32==5 && c.r6.u32==0 && c.r7.u32==1); ++stream_refs;
  } else if(fn==0x8263BBC0) {
    for(int i=0;i<9;++i) WriteFloat(c.r4.u32+i*4,i%4==0?1.f:0.f);
  } else if(fn==0x82479AB0) {
    assert((c.r3.u32==0 || c.r3.u32==3) && c.r4.u32==2 && c.r5.u32==0x82400000);
    assert(c.r8.u32==255 && c.r9.u32==0 && c.r10.u32==0);
    assert(Read32(c.r1.u32+84)==0);
    assert(*static_cast<uint8_t*>(Ptr(c.r1.u32+95))==8);
    ++creates;
    c.r3.u64=0x83100000;
    Write32(0x83100000+68,0x10003); Write32(0x83100000+72,1);
    Write32(0x83100000+784,0); Write32(0x83100000+216,0x4000);
    Write32(0x83100000+228,0x82400000);
    Write32(0x839AB550+5172,0x82402000);
    Write32(kObjectTable+12+3*16,0x83100000);
  } else if(fn==0x822679F8 || fn==0x822678F0) {
    assert(c.r3.u32==0); ++poses;
  } else if(fn==0x823ACF50) {
    assert(c.r3.u32==0x10003); ++destroys;
    Write32(kObjectTable+12+3*16,0);
  } else assert(false);
  return 0;
}

coop::NativePacket ValidPacket() {
  coop::NativePacket p;
  p.sequence=1; p.host_handle=0x12340001;
  float values[]={1,2,3,0,0,0,1};
  std::memcpy(p.payload,values,sizeof(values));
  std::memcpy(p.payload+28,"npc",4);
  p.payload[32]=2; p.payload[33]=0xab; p.payload[34]=0xcd;
  p.payload_size=35;
  return p;
}
int main(int argc,char** argv) {
  auto good=ValidPacket();
  assert(coop::ValidateNpc(good,sizeof(good)));
  assert(!coop::ValidateNpc(good,sizeof(good)-1));
  auto bad=good; bad.payload_size=1025;
  assert(!coop::ValidateNpc(bad,sizeof(bad)));
  bad=good; bad.payload[32]=3;
  assert(!coop::ValidateNpc(bad,sizeof(bad)));
  bad=good; std::memset(bad.payload+28,'x',7);
  assert(!coop::ValidateNpc(bad,sizeof(bad)));
  bad=good; bad.replication_type=5;
  assert(!coop::ValidateNpc(bad,sizeof(bad)));
  bad=good; float nan=std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bad.payload,&nan,4);
  assert(!coop::ValidateNpc(bad,sizeof(bad)));
  bad=good; std::memset(bad.payload+12,0,16);
  assert(!coop::ValidateNpc(bad,sizeof(bad)));

  if(argc>1) {
    unsigned count=0;
    for(const auto& entry:std::filesystem::directory_iterator(argv[1])) {
      if(entry.path().filename().string().starts_with("native-npc-") && entry.path().extension()==".bin") {
        std::ifstream input(entry.path(),std::ios::binary);
        coop::NativePacket captured;
        input.read(reinterpret_cast<char*>(&captured),sizeof(captured));
        const auto got=input.gcount();
        if(got!=sizeof(captured) || input.peek()!=EOF) continue; // older format
        assert(coop::ValidateNpc(captured,size_t(got))); ++count;
      }
    }
    std::printf("Validated %u live game NPC captures\n",count);
  }

  // Mock only the guest engine boundary. Verify construction ABI, preserving
  // guest stack, reuse of our own handle, native pose calls and safe deletion.
  memory.resize(0x2000000);
  WmlApi fake{}; fake.guest_pointer=Ptr; fake.read_u32=Read32;
  fake.write_u32=Write32; fake.write_u16=Write16; fake.write_u8=Write8;
  fake.write_f32=WriteFloat; fake.read_f32=ReadFloat; fake.call=GuestCall; api=&fake;
  Write32(0x82400000+24,0x82401000);
  Write32(0x82400000+608,0x05000012);
  Write32(0x8309ABEC,0x83110000);
  PPCContext context{}; context.r1.u64=0x83200000;
  {
    GuestScratch scratch(reinterpret_cast<WmlContext*>(&context));
    ApplyReplica(scratch,good,0x82400000);
    ApplyReplica(scratch,good,0x82400000);
    assert(creates==1 && replica_count.load()==1 && npc_follow.count==1);
    assert(nudges==1 && stream_refs==1);
    // A recycled slot must never be treated as the replica or destroyed.
    Write32(0x83100000+68,0x20003);
    assert(ResolveHuman(replica_local)==0);
    Write32(0x83100000+68,0x10003);
    DestroyReplica(scratch);
    assert(destroys==1 && replica_local==0 && replica_count.load()==0);
  }
  assert(context.r1.u32==0x83200000);
  for(uint32_t a=0x831fb000;a<0x83200000;++a) assert(*static_cast<uint8_t*>(Ptr(a))==0);

  // Exercise the real network worker over UDP, including the ACK length that
  // was wrong in the previous implementation. Use ephemeral loopback ports.
  WSADATA ws{}; assert(WSAStartup(MAKEWORD(2,2),&ws)==0);
  sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); assert(sock!=INVALID_SOCKET);
  sockaddr_in local{}; local.sin_family=AF_INET; local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(sock,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
  int len=sizeof(local); assert(getsockname(sock,reinterpret_cast<sockaddr*>(&local),&len)==0);
  u_long nonblocking=1; assert(ioctlsocket(sock,FIONBIO,&nonblocking)==0);
  SOCKET client=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); assert(client!=INVALID_SOCKET);
  DWORD timeout=2000; setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
  running=true; hosting=true; stop_thread=false; connected=false; native_out=good;
  // Balance the worker's WSACleanup separately from the test's socket lifetime.
  assert(WSAStartup(MAKEWORD(2,2),&ws)==0);
  net_thread=std::thread([local]{NetworkLoop(local,true);});
  assert(sendto(client,kHello,sizeof(kHello)-1,0,reinterpret_cast<sockaddr*>(&local),sizeof(local))==sizeof(kHello)-1);
  bool ack=false,npc=false;
  char bytes[1500];
  for(int i=0;i<8 && !(ack&&npc);++i) {
    int n=recv(client,bytes,sizeof(bytes),0); assert(n>0);
    if(n==sizeof(kAck)-1 && !std::memcmp(bytes,kAck,n)) ack=true;
    if(n==sizeof(good)) {
      coop::NativePacket received; std::memcpy(&received,bytes,n);
      assert(coop::ValidateNpc(received,n));
      assert(!std::memcmp(&received,&good,n)); npc=true;
    }
  }
  assert(ack&&npc&&connected.load());
  StopSession(); assert(!net_thread.joinable() && sock==INVALID_SOCKET);
  closesocket(client);

  SOCKET fake_host=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  local.sin_port=0;
  assert(bind(fake_host,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
  assert(getsockname(fake_host,reinterpret_cast<sockaddr*>(&local),&len)==0);
  setsockopt(fake_host,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
  sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  assert(ioctlsocket(sock,FIONBIO,&nonblocking)==0);
  running=true; hosting=false; connected=false; native_received=0; native_pending=false;
  assert(WSAStartup(MAKEWORD(2,2),&ws)==0);
  net_thread=std::thread([local]{NetworkLoop(local,false);});
  sockaddr_in joining{}; len=sizeof(joining);
  int n=recvfrom(fake_host,bytes,sizeof(bytes),0,reinterpret_cast<sockaddr*>(&joining),&len);
  assert(n==sizeof(kHello)-1 && !std::memcmp(bytes,kHello,n));
  sendto(fake_host,kAck,sizeof(kAck)-1,0,reinterpret_cast<sockaddr*>(&joining),sizeof(joining));
  sendto(fake_host,reinterpret_cast<char*>(&bad),sizeof(bad),0,reinterpret_cast<sockaddr*>(&joining),sizeof(joining));
  for(int i=0;i<2;++i) sendto(fake_host,reinterpret_cast<char*>(&good),sizeof(good),0,reinterpret_cast<sockaddr*>(&joining),sizeof(joining));
  for(int i=0;i<100 && !native_received.load();++i) Sleep(5);
  Sleep(30);
  assert(connected.load() && native_received.load()==1);
  { std::lock_guard lock(probe_mutex); assert(native_pending && !std::memcmp(&native_in,&good,sizeof(good))); }
  StopSession(); closesocket(fake_host); WSACleanup();
  std::puts("PASS: payloads, native actor ABI/lifecycle mocks, UDP host/client workers, duplicate rejection, shutdown");
}
