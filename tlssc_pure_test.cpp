#include "tlssc_pure.h"
#ifdef TLSSC_FREESTANDING_TEST
#define assert(expr) do { if (!(expr)) return __LINE__; } while (0)
extern "C" void* memset(void* dst, int value, unsigned long long size) {
  unsigned char* p = static_cast<unsigned char*>(dst);
  for (unsigned long long i = 0; i < size; ++i) p[i] = static_cast<unsigned char>(value);
  return dst;
}
#else
#include <cassert>
#include <cstdio>
#endif
using namespace tlssc;
int main() {
  Config c; State s; uint8_t ap[8] = {}; uint8_t road[8] = {}; uint8_t frame[8] = {0x80,1,2,3,0x15,5,6,7};
  assert(!c.enabled && !c.block_highway && !c.block_noa);
  assert(!observe_ap(s, nullptr, 8) && !observe_ap(s, ap, 5));
  for (unsigned i=0; i<16; ++i) { ap[0]=uint8_t(0xa0 | i); assert(observe_ap(s,ap,6)); assert(ap_active(s)==(i>=3 && i<=6)); assert(noa_active(s)==(i==5)); }
  ap[0]=3; observe_ap(s,ap,8); c.enabled=true;
  auto d=prepare(s,c,100,0x3fd,false,false,frame,8); assert(d.action==Action::enable && d.data[4]==0xd5);
  for (int i=0;i<8;++i) if(i!=4) assert(d.data[i]==frame[i]);
  complete(s,d.action,false); assert(!s.injected); complete(s,d.action,true); assert(s.injected);
  uint8_t stock[8]; for (int i=0;i<8;++i) stock[i]=frame[i]; stock[4]|=0xc0;
  assert(prepare(s,c,100,0x3fd,false,false,stock,8).action==Action::none);
  assert(prepare(s,c,100,0x3fd,false,false,nullptr,8).action==Action::none);
  assert(prepare(s,c,100,0x3fd,false,false,frame,7).action==Action::none);
  assert(prepare(s,c,100,0x3fc,false,false,frame,8).action==Action::none);
  assert(prepare(s,c,100,0x3fd,true,false,frame,8).action==Action::none);
  assert(prepare(s,c,100,0x3fd,false,true,frame,8).action==Action::none);
  frame[0]=1; assert(prepare(s,c,100,0x3fd,false,false,frame,8).action==Action::none); frame[0]=0x80;
  ap[0]=5; observe_ap(s,ap,8); assert(prepare(s,c,100,0x3fd,false,false,frame,8).action==Action::enable);
  c.block_noa=true; d=prepare(s,c,100,0x3fd,false,false,stock,8); assert(d.action==Action::clear && d.data[4]==0x15 && s.clear_pending);
  complete(s,d.action,false); assert(s.injected && s.clear_pending);
  d=prepare(s,c,101,0x3fd,false,false,frame,8); assert(d.action==Action::clear); complete(s,d.action,true); assert(!s.injected && !s.clear_pending);
  c.block_noa=false; d=prepare(s,c,102,0x3fd,false,false,frame,8); complete(s,d.action,true); c.enabled=false;
  d=prepare(s,c,103,0x3fd,false,false,frame,8); assert(d.action==Action::clear); complete(s,d.action,false);
  c.enabled=true; d=prepare(s,c,104,0x3fd,false,false,stock,8); assert(d.action==Action::none && !s.clear_pending && s.injected);
  road[0]=0x28; road[3]=0x30; road[4]=0xe0; Road r=decode_road(road,5);
  assert(r.valid && r.road_class==5 && r.gps_match && r.nav_route && r.controlled_access && r.left_off_ramp && r.right_off_ramp);
  assert(!decode_road(nullptr,8).valid && !observe_road(s,road,4,100));
  observe_road(s,road,5,100); assert(!s.highway); observe_road(s,road,5,101); assert(s.highway);
  assert(!highway_blocked(c,s,101)); c.block_highway=true;
  assert(highway_blocked(c,s,2101) && !highway_blocked(c,s,2102));
  d=prepare(s,c,101,0x3fd,false,false,stock,8); assert(d.action==Action::clear);
  road[4]=0; observe_road(s,road,5,102); assert(s.highway); observe_road(s,road,5,103); assert(!s.highway);
  road[4]=0x20; observe_road(s,road,5,104); observe_road(s,road,5,105); assert(s.highway);
  road[3]=0; observe_road(s,road,5,106); assert(!s.highway && !s.positive && !s.negative);
  road[3]=0x10; observe_road(s,road,5,0xfffffffeu); observe_road(s,road,5,0xffffffffu); assert(highway_blocked(c,s,10));
  observe_road(s,road,5,0); assert(!highway_blocked(c,s,1));
#ifndef TLSSC_FREESTANDING_TEST
  std::puts("tlssc pure tests passed");
#endif
  return 0;
}
